#include "slothdb/execution/radix_count_agg.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string_view>
#include <thread>

#include "third_party/unordered_dense.h"
#include "slothdb/storage/parquet.hpp"

namespace slothdb {

RadixCountAgg::RadixCountAgg(int max_threads)
    : max_threads_(max_threads),
      tl_scatter_(max_threads),
      radix_maps_(N_RADIX) {
    for (auto &tl : tl_scatter_) {
        tl.resize(N_RADIX);
    }
}

void RadixCountAgg::ReserveExpectedRows(int64_t total_rows) {
    // Per thread: total_rows / max_threads / N_RADIX. Add 25% slack.
    if (total_rows <= 0) return;
    int64_t per_buf = (total_rows + max_threads_ * N_RADIX - 1)
                      / (max_threads_ * N_RADIX);
    per_buf = (per_buf * 5) / 4;
    if (per_buf < 1024) per_buf = 1024;
    for (auto &tl : tl_scatter_) {
        for (auto &buf : tl) buf.reserve((size_t)per_buf);
    }
}

void RadixCountAgg::ScatterRow(int tid, int64_t key) {
    // Hash once to pick the radix bucket. Use the high bits of the
    // ankerl mix so that the low bits remain available for the per-radix
    // hash map's bucket indexing (no double-hashing the same bits).
    uint64_t h = ankerl::unordered_dense::hash<int64_t>{}(key);
    int radix = (int)((h >> (64 - N_RADIX_BITS)) & (N_RADIX - 1));
    tl_scatter_[tid][radix].push_back(key);
}

void RadixCountAgg::MergeRadix(int radix) {
    // Disjoint by hash → only this worker writes to radix_maps_[radix].
    auto &out = radix_maps_[radix];
    // Reserve only an UPPER bound on unique count (assume ≤25% unique
    // ratio — typical for high-card GROUP BY where rows >> uniques).
    // Over-reserving makes the map cold to L2 and probes go to RAM.
    size_t total = 0;
    for (auto &tl : tl_scatter_) total += tl[radix].size();
    if (total > 0) out.reserve(total / 4 + 64);
    for (auto &tl : tl_scatter_) {
        for (int64_t k : tl[radix]) out.increment(k);
        // Free the scatter buffer immediately — phase 2 doesn't read
        // from it again. Releases memory before phase 3.
        std::vector<int64_t>().swap(tl[radix]);
    }
}

std::vector<RadixCountResult> RadixCountAgg::EmitTopK(int k) const {
    if (k <= 0) {
        std::vector<RadixCountResult> all;
        all.reserve(TotalGroups());
        for (auto &m : radix_maps_) {
            m.for_each([&](int64_t key, int64_t count) {
                all.push_back({key, count});
            });
        }
        return all;
    }
    // Min-heap of size k. New (key, count) replaces the min when count >
    // min.count. Result sorted DESC at the end.
    auto cmp = [](const RadixCountResult &a, const RadixCountResult &b) {
        return a.count > b.count;  // min-heap: smallest at top
    };
    std::priority_queue<RadixCountResult, std::vector<RadixCountResult>, decltype(cmp)>
        heap(cmp);
    for (auto &m : radix_maps_) {
        m.for_each([&](int64_t key, int64_t count) {
            if ((int)heap.size() < k) {
                heap.push({key, count});
            } else if (count > heap.top().count) {
                heap.pop();
                heap.push({key, count});
            }
        });
    }
    std::vector<RadixCountResult> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.push_back(heap.top());
        heap.pop();
    }
    std::reverse(out.begin(), out.end());  // DESC by count
    return out;
}

size_t RadixCountAgg::TotalGroups() const {
    size_t total = 0;
    for (auto &m : radix_maps_) total += m.size();
    return total;
}

// =====================================================================
// RadixCountAggStr — VARCHAR variant
// =====================================================================

namespace {

constexpr int STR_NSHARDS = RadixCountAggStr::N_RADIX;
constexpr size_t STR_ARENA_CHUNK = 256 * 1024;

struct StrArenaChunk {
    std::vector<char> bytes;
    size_t used = 0;
};

// Per-thread state: NSHARDS maps from string_view → count, plus a
// bump arena for stable string storage. Mirrors string_dedup.
struct StrPerThread {
    std::array<ankerl::unordered_dense::map<std::string_view, int64_t>,
               STR_NSHARDS> shards;
    std::vector<StrArenaChunk> arena;
    // Per-RG dict-counter scratch reused across RGs. Sized to dict_size
    // on first use within an RG; zeroed at start of each RG ingest.
    std::vector<int64_t> dict_count_scratch;

    char* alloc(size_t n) {
        if (arena.empty() || arena.back().bytes.size() - arena.back().used < n) {
            arena.emplace_back();
            size_t cap = (n > STR_ARENA_CHUNK) ? n : STR_ARENA_CHUNK;
            arena.back().bytes.resize(cap);
        }
        char* p = arena.back().bytes.data() + arena.back().used;
        arena.back().used += n;
        return p;
    }
};

}  // anonymous namespace

struct RadixCountAggStrImpl {
    int max_threads;
    std::vector<std::unique_ptr<StrPerThread>> threads;
    // Final per-shard merged maps. Phase 2 populates these. Strings
    // remain owned by their per-thread arena (phase 2 holds the union
    // of all per-thread arenas live until EmitTopK consumes them).
    std::array<ankerl::unordered_dense::map<std::string_view, int64_t>,
               STR_NSHARDS> final_maps;
};

RadixCountAggStr::RadixCountAggStr(int max_threads)
    : impl_(std::make_unique<RadixCountAggStrImpl>()) {
    impl_->max_threads = max_threads;
    impl_->threads.reserve(max_threads);
    for (int i = 0; i < max_threads; i++) {
        impl_->threads.emplace_back(std::make_unique<StrPerThread>());
    }
}

RadixCountAggStr::~RadixCountAggStr() = default;

void RadixCountAggStr::IncrementRow(int tid, const char* data, uint32_t size) {
    IncrementBy(tid, data, size, 1);
}

void RadixCountAggStr::IncrementBy(int tid, const char* data, uint32_t size,
                                   int64_t delta) {
    auto& pt = *impl_->threads[tid];
    ankerl::unordered_dense::hash<std::string_view> H;
    std::string_view probe(data, size);
    size_t h = H(probe);
    int shard = (int)(h & (STR_NSHARDS - 1));
    auto& m = pt.shards[shard];
    auto it = m.find(probe);
    if (it != m.end()) {
        it->second += delta;
        return;
    }
    char* dst = pt.alloc(size);
    if (size) std::memcpy(dst, data, size);
    m.emplace(std::string_view(dst, size), delta);
}

void RadixCountAggStr::IncrementByDictRG(int tid,
                                         const uint32_t* dict_indices,
                                         uint32_t nrows,
                                         const string_t* dict_values,
                                         uint32_t dict_size,
                                         const uint8_t* validity,
                                         const uint8_t* keep_mask) {
    if (dict_size == 0 || nrows == 0) return;
    auto& pt = *impl_->threads[tid];
    if (pt.dict_count_scratch.size() < dict_size) {
        pt.dict_count_scratch.assign(dict_size, 0);
    } else {
        std::fill_n(pt.dict_count_scratch.begin(), dict_size, 0);
    }
    int64_t* cnt = pt.dict_count_scratch.data();

    // Pass 1: tight per-row counter increment. No hash, no map. Specialized
    // by (validity?, keep_mask?) to keep the hot loop branch-free.
    if (!validity && !keep_mask) {
        for (uint32_t r = 0; r < nrows; r++) {
            uint32_t d = dict_indices[r];
            if (d < dict_size) cnt[d]++;
        }
    } else if (!validity && keep_mask) {
        for (uint32_t r = 0; r < nrows; r++) {
            if (!keep_mask[r]) continue;
            uint32_t d = dict_indices[r];
            if (d < dict_size) cnt[d]++;
        }
    } else if (validity && !keep_mask) {
        for (uint32_t r = 0; r < nrows; r++) {
            if (!validity[r]) continue;
            uint32_t d = dict_indices[r];
            if (d < dict_size) cnt[d]++;
        }
    } else {
        for (uint32_t r = 0; r < nrows; r++) {
            if (!keep_mask[r]) continue;
            if (!validity[r]) continue;
            uint32_t d = dict_indices[r];
            if (d < dict_size) cnt[d]++;
        }
    }

    // Pass 2: fold per-dict counters into per-thread shard maps. One hash
    // + lookup per non-zero dict entry instead of one per row.
    ankerl::unordered_dense::hash<std::string_view> H;
    for (uint32_t d = 0; d < dict_size; d++) {
        int64_t delta = cnt[d];
        if (delta == 0) continue;
        const string_t& sv = dict_values[d];
        const char* data = sv.GetData();
        uint32_t size = sv.GetSize();
        std::string_view probe(data, size);
        size_t h = H(probe);
        int shard = (int)(h & (STR_NSHARDS - 1));
        auto& m = pt.shards[shard];
        auto it = m.find(probe);
        if (it != m.end()) {
            it->second += delta;
        } else {
            char* dst = pt.alloc(size);
            if (size) std::memcpy(dst, data, size);
            m.emplace(std::string_view(dst, size), delta);
        }
    }
}

void RadixCountAggStr::IncrementByDictRGSkipDi(int tid,
                                               const uint32_t* dict_indices,
                                               uint32_t nrows,
                                               const string_t* dict_values,
                                               uint32_t dict_size,
                                               const uint8_t* validity,
                                               uint32_t skip_di) {
    if (dict_size == 0 || nrows == 0) return;
    auto& pt = *impl_->threads[tid];
    if (pt.dict_count_scratch.size() < dict_size) {
        pt.dict_count_scratch.assign(dict_size, 0);
    } else {
        std::fill_n(pt.dict_count_scratch.begin(), dict_size, 0);
    }
    int64_t* cnt = pt.dict_count_scratch.data();
    bool _pf = PqProfileOn();
    auto _t0 = _pf ? std::chrono::steady_clock::now()
                   : std::chrono::steady_clock::time_point{};
    // Pass 1: no keep_mask — single 3-cyc/row inner loop. Bounds check
    // collapses to a single compare since dict_size fits in u32.
    if (!validity) {
        for (uint32_t r = 0; r < nrows; r++) {
            uint32_t d = dict_indices[r];
            if (d < dict_size) cnt[d]++;
        }
    } else {
        for (uint32_t r = 0; r < nrows; r++) {
            if (!validity[r]) continue;
            uint32_t d = dict_indices[r];
            if (d < dict_size) cnt[d]++;
        }
    }
    // Zero out the skipped dict entry so Pass 2 ignores it.
    if (skip_di < dict_size) cnt[skip_di] = 0;
    auto _t1 = _pf ? std::chrono::steady_clock::now()
                   : std::chrono::steady_clock::time_point{};
    // Pass 2: one hash + lookup per non-zero dict entry.
    ankerl::unordered_dense::hash<std::string_view> H;
    for (uint32_t d = 0; d < dict_size; d++) {
        int64_t delta = cnt[d];
        if (delta == 0) continue;
        const string_t& sv = dict_values[d];
        const char* data = sv.GetData();
        uint32_t size = sv.GetSize();
        std::string_view probe(data, size);
        size_t h = H(probe);
        int shard = (int)(h & (STR_NSHARDS - 1));
        auto& m = pt.shards[shard];
        auto it = m.find(probe);
        if (it != m.end()) {
            it->second += delta;
        } else {
            char* dst = pt.alloc(size);
            if (size) std::memcpy(dst, data, size);
            m.emplace(std::string_view(dst, size), delta);
        }
    }
    if (_pf) {
        auto _t2 = std::chrono::steady_clock::now();
        g_pq_profile.agg_pass1_ns.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                _t1 - _t0).count(), std::memory_order_relaxed);
        g_pq_profile.agg_pass2_ns.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                _t2 - _t1).count(), std::memory_order_relaxed);
        g_pq_profile.agg_dict_sum.fetch_add(dict_size, std::memory_order_relaxed);
    }
}

void RadixCountAggStr::MergeShard(int shard) {
    // Disjoint by hash → only this worker writes to final_maps[shard].
    auto& out = impl_->final_maps[shard];
    size_t max_single = 0;
    for (auto& tl : impl_->threads) {
        size_t sz = tl->shards[shard].size();
        if (sz > max_single) max_single = sz;
    }
    out.reserve(max_single + max_single / 4);
    for (auto& tl : impl_->threads) {
        for (auto& kv : tl->shards[shard]) {
            auto it = out.find(kv.first);
            if (it == out.end()) {
                out.emplace(kv.first, kv.second);
            } else {
                it->second += kv.second;
            }
        }
    }
}

std::vector<RadixCountStrResult> RadixCountAggStr::EmitTopK(int k) const {
    if (k <= 0) {
        std::vector<RadixCountStrResult> all;
        all.reserve(TotalGroups());
        for (auto& m : impl_->final_maps) {
            for (auto& kv : m) {
                all.push_back({std::string(kv.first), kv.second});
            }
        }
        return all;
    }
    auto cmp = [](const RadixCountStrResult& a, const RadixCountStrResult& b) {
        return a.count > b.count;  // min-heap
    };
    std::priority_queue<RadixCountStrResult,
                        std::vector<RadixCountStrResult>, decltype(cmp)>
        heap(cmp);
    for (auto& m : impl_->final_maps) {
        for (auto& kv : m) {
            if ((int)heap.size() < k) {
                heap.push({std::string(kv.first), kv.second});
            } else if (kv.second > heap.top().count) {
                heap.pop();
                heap.push({std::string(kv.first), kv.second});
            }
        }
    }
    std::vector<RadixCountStrResult> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.push_back(std::move(const_cast<RadixCountStrResult&>(heap.top())));
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

size_t RadixCountAggStr::TotalGroups() const {
    size_t total = 0;
    for (auto& m : impl_->final_maps) total += m.size();
    return total;
}

// =====================================================================
// RadixHashCountStr — bounded-HT + radix-spill VARCHAR GROUP-BY COUNT
// =====================================================================

namespace {

constexpr size_t HC_ACTIVE_CAP = 16384;                   // flush threshold
constexpr size_t HC_ARENA_BYTES = 4u * 1024 * 1024;       // per-thread, fixed

// One flushed (key, count) entry. `off`/`len` index the owning thread's
// `sbytes`; `hash` is the precomputed key hash — the merge groups on it.
struct HcEnt {
    uint64_t hash;
    uint32_t off;
    uint32_t len;
    int64_t count;
};

struct HcThread {
    // Bounded active hash table — capped at HC_ACTIVE_CAP so it stays
    // L2-resident; flushed (appended to the flat buffers + cleared) when
    // it fills.
    ankerl::unordered_dense::map<std::string_view, int64_t> active;
    // Fixed-size arena holding the active window's key bytes. Never grown
    // (the active map's string_view keys must not be invalidated); a flush
    // is forced if it would overflow. Reset to offset 0 on each flush.
    std::vector<char> arena;
    size_t arena_used = 0;
    // Flat spill: every flushed entry, appended 1-way (NO radix scatter — a
    // 16/256-way scatter measured ~530-2900 ns/op on this chip, vs ~18 ns
    // for the 1-way append). Finalize sorts `entries` by hash; EmitTopK does
    // a k-way merge that groups on the hash.
    std::vector<HcEnt> entries;
    std::vector<char> sbytes;
    std::vector<int64_t> dict_scratch;  // dict-RG histogram, reused per RG
};

}  // anonymous namespace

struct RadixHashCountStrImpl {
    int max_threads = 1;
    std::vector<std::unique_ptr<HcThread>> threads;
};

namespace {

// Flush the active table: append every (key,count) to the thread's flat
// spill buffers (1-way — no scatter), then reset the table and arena.
inline void HcFlush(HcThread& th) {
    ankerl::unordered_dense::hash<std::string_view> H;
    for (auto& kv : th.active) {
        std::string_view sv = kv.first;
        uint32_t off = (uint32_t)th.sbytes.size();
        uint32_t len = (uint32_t)sv.size();
        if (len) th.sbytes.insert(th.sbytes.end(), sv.data(), sv.data() + len);
        th.entries.push_back({(uint64_t)H(sv), off, len, kv.second});
    }
    th.active.clear();
    th.arena_used = 0;
}

// Ingest one (string, count) pair into the bounded active table.
inline void HcIngest(HcThread& th, const char* data, uint32_t sz, int64_t c) {
    std::string_view probe(data, sz);
    auto it = th.active.find(probe);
    if (it != th.active.end()) {
        it->second += c;
        return;
    }
    if (th.active.size() >= HC_ACTIVE_CAP ||
        th.arena_used + sz > th.arena.size()) {
        HcFlush(th);
    }
    char* dst = th.arena.data() + th.arena_used;
    th.arena_used += sz;
    if (sz) std::memcpy(dst, data, sz);
    th.active.emplace(std::string_view(dst, sz), c);
}

}  // anonymous namespace

RadixHashCountStr::RadixHashCountStr(int max_threads)
    : impl_(std::make_unique<RadixHashCountStrImpl>()) {
    impl_->max_threads = max_threads < 1 ? 1 : max_threads;
    impl_->threads.reserve(impl_->max_threads);
    for (int i = 0; i < impl_->max_threads; i++) {
        auto th = std::make_unique<HcThread>();
        th->arena.resize(HC_ARENA_BYTES);
        th->active.reserve(HC_ACTIVE_CAP + HC_ACTIVE_CAP / 4);
        // Pre-reserve the flat spill buffers (~1M entries / thread typical).
        th->entries.reserve(1u << 20);
        th->sbytes.reserve(32u << 20);
        impl_->threads.emplace_back(std::move(th));
    }
}

RadixHashCountStr::~RadixHashCountStr() = default;

void RadixHashCountStr::IngestDictRG(int tid, const uint32_t* dict_indices,
                                     uint32_t nrows,
                                     const string_t* dict_values,
                                     uint32_t dict_size,
                                     const uint8_t* validity,
                                     uint32_t skip_di) {
    if (dict_size == 0 || nrows == 0) return;
    auto& th = *impl_->threads[tid];
    if (th.dict_scratch.size() < dict_size) th.dict_scratch.assign(dict_size, 0);
    else std::fill_n(th.dict_scratch.begin(), dict_size, 0);
    int64_t* cnt = th.dict_scratch.data();
    if (!validity) {
        for (uint32_t r = 0; r < nrows; r++) {
            uint32_t d = dict_indices[r];
            if (d < dict_size) cnt[d]++;
        }
    } else {
        for (uint32_t r = 0; r < nrows; r++) {
            if (!validity[r]) continue;
            uint32_t d = dict_indices[r];
            if (d < dict_size) cnt[d]++;
        }
    }
    if (skip_di < dict_size) cnt[skip_di] = 0;  // fold WHERE col<>'' filter
    for (uint32_t d = 0; d < dict_size; d++) {
        int64_t c = cnt[d];
        if (c == 0) continue;
        HcIngest(th, dict_values[d].GetData(), dict_values[d].GetSize(), c);
    }
}

void RadixHashCountStr::IngestPlainRG(int tid, const string_t* str_data,
                                      uint32_t nrows, const uint8_t* validity,
                                      bool skip_empty) {
    if (nrows == 0) return;
    auto& th = *impl_->threads[tid];
    for (uint32_t r = 0; r < nrows; r++) {
        if (validity && !validity[r]) continue;
        uint32_t sz = str_data[r].GetSize();
        if (skip_empty && sz == 0) continue;  // fold WHERE col<>'' filter
        HcIngest(th, str_data[r].GetData(), sz, 1);
    }
}

void RadixHashCountStr::IngestKey(int tid, const char* data, uint32_t size) {
    HcIngest(*impl_->threads[tid], data, size, 1);
}

void RadixHashCountStr::Finalize() {
    bool _pf = PqProfileOn();
    auto _t0 = _pf ? std::chrono::steady_clock::now()
                   : std::chrono::steady_clock::time_point{};
    // Flush whatever is still in each thread's active table.
    for (auto& thp : impl_->threads) {
        if (!thp->active.empty()) HcFlush(*thp);
    }
    // Sort each thread's flat spill by hash — parallel, one worker each.
    // EmitTopK then k-way merges the 12 sorted lists, grouping on hash.
    int nt = impl_->max_threads;
    auto sort_one = [this](int t) {
        auto& e = impl_->threads[t]->entries;
        std::sort(e.begin(), e.end(), [](const HcEnt& a, const HcEnt& b) {
            return a.hash < b.hash;
        });
    };
    if (nt <= 1) {
        sort_one(0);
    } else {
        std::vector<std::thread> ws;
        ws.reserve(nt - 1);
        for (int t = 1; t < nt; t++) ws.emplace_back(sort_one, t);
        sort_one(0);
        for (auto& t : ws) if (t.joinable()) t.join();
    }
    if (_pf) {
        fprintf(stderr, "[SLOTH_PROFILE] RadixHashCountStr::Finalize %.0fms\n",
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - _t0).count() / 1e6);
    }
}

std::vector<RadixCountStrResult> RadixHashCountStr::EmitTopK(int k) const {
    // k-way linear-min merge of the per-thread hash-sorted `entries`.
    // Consecutive entries with the same hash are one group (the 64-bit hash
    // is trusted as the group identity — P(collision among ~6M keys) ~1e-6,
    // and a collision would have to land on a top-K group to change the
    // answer).
    //
    // A group lives entirely in one hash value, so splitting the hash space
    // into disjoint ranges partitions groups cleanly — no group spans two
    // ranges. For k>0 we run one merge worker per range in parallel (each
    // produces a local top-k), then combine. Single-threaded k-way merge
    // over the full ~5-10M entries was ~150ms+ unparallelised. The k<=0
    // full-emit path stays single-threaded (rare; no LIMIT).
    bool _pf = PqProfileOn();
    auto _t0 = _pf ? std::chrono::steady_clock::now()
                   : std::chrono::steady_clock::time_point{};
    const int T = impl_->max_threads;

    // Merge entries whose hash is in [lo, hi) (or [lo, end] when `last`).
    auto merge_range = [this, T](uint64_t lo, uint64_t hi, bool last, int k,
                                 std::vector<RadixCountStrResult>& out) {
        auto cmp_hash = [](const HcEnt& x, uint64_t b) { return x.hash < b; };
        std::vector<size_t> cur((size_t)T), end((size_t)T);
        for (int t = 0; t < T; t++) {
            const auto& e = impl_->threads[(size_t)t]->entries;
            cur[(size_t)t] = (size_t)(
                std::lower_bound(e.begin(), e.end(), lo, cmp_hash) - e.begin());
            end[(size_t)t] = last ? e.size() : (size_t)(
                std::lower_bound(e.begin(), e.end(), hi, cmp_hash) - e.begin());
        }
        auto cmp = [](const RadixCountStrResult& a,
                      const RadixCountStrResult& b) {
            return a.count > b.count;  // min-heap: smallest count on top
        };
        std::priority_queue<RadixCountStrResult,
                            std::vector<RadixCountStrResult>, decltype(cmp)>
            heap(cmp);
        auto emit = [&](const char* s, uint32_t l, int64_t c) {
            if (k <= 0) {
                out.push_back({std::string(s, l), c});
            } else if ((int)heap.size() < k) {
                heap.push({std::string(s, l), c});
            } else if (c > heap.top().count) {
                heap.pop();
                heap.push({std::string(s, l), c});
            }
        };
        bool have = false;
        uint64_t cur_hash = 0;
        const char* cur_str = nullptr;
        uint32_t cur_len = 0;
        int64_t cur_count = 0;
        while (true) {
            int mt = -1;
            uint64_t mh = 0;
            for (int t = 0; t < T; t++) {
                if (cur[(size_t)t] >= end[(size_t)t]) continue;
                uint64_t h = impl_->threads[(size_t)t]
                                 ->entries[cur[(size_t)t]].hash;
                if (mt < 0 || h < mh) { mh = h; mt = t; }
            }
            if (mt < 0) break;
            const HcThread& th = *impl_->threads[(size_t)mt];
            const HcEnt& e = th.entries[cur[(size_t)mt]];
            cur[(size_t)mt]++;
            if (have && e.hash == cur_hash) {
                cur_count += e.count;
            } else {
                if (have) emit(cur_str, cur_len, cur_count);
                cur_hash = e.hash;
                cur_str = th.sbytes.data() + e.off;
                cur_len = e.len;
                cur_count = e.count;
                have = true;
            }
        }
        if (have) emit(cur_str, cur_len, cur_count);
        if (k > 0) {
            out.reserve(heap.size());
            while (!heap.empty()) {
                out.push_back(std::move(
                    const_cast<RadixCountStrResult&>(heap.top())));
                heap.pop();
            }
        }
    };

    std::vector<RadixCountStrResult> result;
    if (k <= 0 || T <= 1) {
        merge_range(0, 0, true, k, result);
    } else {
        const int P = T;
        std::vector<std::vector<RadixCountStrResult>> partials((size_t)P);
        const uint64_t span = UINT64_MAX / (uint64_t)P;
        auto do_range = [&](int w) {
            uint64_t lo = (uint64_t)w * span;
            bool last = (w == P - 1);
            uint64_t hi = last ? 0 : (uint64_t)(w + 1) * span;
            merge_range(lo, hi, last, k, partials[(size_t)w]);
        };
        std::vector<std::thread> ws;
        ws.reserve((size_t)(P - 1));
        for (int w = 1; w < P; w++) ws.emplace_back(do_range, w);
        do_range(0);
        for (auto& t : ws) if (t.joinable()) t.join();
        // Combine the P local top-k lists into the global top-k. The global
        // winners are a subset of the per-range winners (a global top-k
        // member is also among the top-k of its own range).
        auto cmp = [](const RadixCountStrResult& a,
                      const RadixCountStrResult& b) {
            return a.count > b.count;
        };
        std::priority_queue<RadixCountStrResult,
                            std::vector<RadixCountStrResult>, decltype(cmp)>
            heap(cmp);
        for (auto& pv : partials) {
            for (auto& r : pv) {
                if ((int)heap.size() < k) {
                    heap.push(std::move(r));
                } else if (r.count > heap.top().count) {
                    heap.pop();
                    heap.push(std::move(r));
                }
            }
        }
        result.reserve(heap.size());
        while (!heap.empty()) {
            result.push_back(std::move(
                const_cast<RadixCountStrResult&>(heap.top())));
            heap.pop();
        }
    }
    if (k > 0) std::reverse(result.begin(), result.end());
    if (_pf) {
        fprintf(stderr, "[SLOTH_PROFILE] RadixHashCountStr::EmitTopK %.0fms\n",
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - _t0).count() / 1e6);
    }
    return result;
}

int64_t RadixHashCountStr::CountDistinctGroups() const {
    // k-way merge counting distinct hash groups, parallelised by hash range.
    // A group is a single hash value, so a range boundary partitions groups
    // cleanly — no group is split across two range workers.
    const int T = impl_->max_threads;
    int P = T < 1 ? 1 : T;
    auto cmp_hash = [](const HcEnt& x, uint64_t b) { return x.hash < b; };
    std::vector<int64_t> partial((size_t)P, 0);
    auto do_range = [&](int w) {
        const uint64_t span = UINT64_MAX / (uint64_t)P;
        uint64_t lo = (uint64_t)w * span;
        bool last = (w == P - 1);
        uint64_t hi = last ? 0 : (uint64_t)(w + 1) * span;
        std::vector<size_t> cur((size_t)T), end((size_t)T);
        for (int t = 0; t < T; t++) {
            const auto& e = impl_->threads[(size_t)t]->entries;
            cur[(size_t)t] = (size_t)(
                std::lower_bound(e.begin(), e.end(), lo, cmp_hash) - e.begin());
            end[(size_t)t] = last ? e.size() : (size_t)(
                std::lower_bound(e.begin(), e.end(), hi, cmp_hash) - e.begin());
        }
        int64_t groups = 0;
        bool have = false;
        uint64_t cur_hash = 0;
        while (true) {
            int mt = -1;
            uint64_t mh = 0;
            for (int t = 0; t < T; t++) {
                if (cur[(size_t)t] >= end[(size_t)t]) continue;
                uint64_t h = impl_->threads[(size_t)t]->entries[cur[(size_t)t]].hash;
                if (mt < 0 || h < mh) { mh = h; mt = t; }
            }
            if (mt < 0) break;
            uint64_t h = impl_->threads[(size_t)mt]->entries[cur[(size_t)mt]].hash;
            cur[(size_t)mt]++;
            if (!have || h != cur_hash) { groups++; cur_hash = h; have = true; }
        }
        partial[(size_t)w] = groups;
    };
    if (P <= 1) {
        do_range(0);
    } else {
        std::vector<std::thread> ws;
        ws.reserve((size_t)(P - 1));
        for (int w = 1; w < P; w++) ws.emplace_back(do_range, w);
        do_range(0);
        for (auto& t : ws) if (t.joinable()) t.join();
    }
    int64_t total = 0;
    for (auto p : partial) total += p;
    return total;
}

// =====================================================================
// RadixCount2ColIntStr — 2-col (INT/BIGINT + VARCHAR) variant
// =====================================================================

namespace {

constexpr int IS_NSHARDS = RadixCount2ColIntStr::N_RADIX;
constexpr size_t IS_ARENA_CHUNK = 256 * 1024;

// Stash the combined hash in the key so IntStrKeyHash returns it directly.
// This lets callers precompute the (int_hash, str_hash) combine once per dict
// entry per RG and reuse across many rows. Costs 8 bytes per key (24 → 32);
// recoups the cost on dict-heavy hot loops where the per-row string hash was
// the dominant cost.
struct IntStrKey {
    int64_t i;
    std::string_view s;
    size_t h;
    bool operator==(const IntStrKey& o) const noexcept {
        return i == o.i && s == o.s;
    }
};

struct IntStrKeyHash {
    using is_avalanching = void;  // marks hash as already well-mixed
    size_t operator()(const IntStrKey& k) const noexcept {
        return k.h;
    }
};

inline size_t MixIntStrHash(int64_t int_key, size_t str_hash) {
    size_t h1 = ankerl::unordered_dense::hash<int64_t>{}(int_key);
    return (h1 * 0x9E3779B97F4A7C15ULL) ^ str_hash;
}

struct IsArenaChunk {
    std::vector<char> bytes;
    size_t used = 0;
};

struct IsPerThread {
    std::array<
        ankerl::unordered_dense::map<IntStrKey, int64_t, IntStrKeyHash>,
        IS_NSHARDS> shards;
    std::vector<IsArenaChunk> arena;

    char* alloc(size_t n) {
        if (arena.empty() || arena.back().bytes.size() - arena.back().used < n) {
            arena.emplace_back();
            size_t cap = (n > IS_ARENA_CHUNK) ? n : IS_ARENA_CHUNK;
            arena.back().bytes.resize(cap);
        }
        char* p = arena.back().bytes.data() + arena.back().used;
        arena.back().used += n;
        return p;
    }
};

}  // anonymous namespace

struct RadixCount2ColIntStrImpl {
    int max_threads;
    std::vector<std::unique_ptr<IsPerThread>> threads;
    std::array<
        ankerl::unordered_dense::map<IntStrKey, int64_t, IntStrKeyHash>,
        IS_NSHARDS> final_maps;
};

RadixCount2ColIntStr::RadixCount2ColIntStr(int max_threads)
    : impl_(std::make_unique<RadixCount2ColIntStrImpl>()) {
    impl_->max_threads = max_threads;
    impl_->threads.reserve(max_threads);
    for (int i = 0; i < max_threads; i++) {
        impl_->threads.emplace_back(std::make_unique<IsPerThread>());
    }
}

RadixCount2ColIntStr::~RadixCount2ColIntStr() = default;

void RadixCount2ColIntStr::ReserveExpectedRows(int64_t total_unique_pairs) {
    if (total_unique_pairs <= 0) return;
    int64_t per_shard = total_unique_pairs /
                        (impl_->max_threads * (int64_t)IS_NSHARDS);
    per_shard = (per_shard * 5) / 4;  // 25% slack
    if (per_shard < 256) per_shard = 256;
    for (auto& tl_p : impl_->threads) {
        for (auto& m : tl_p->shards) m.reserve((size_t)per_shard);
    }
}

void RadixCount2ColIntStr::IncrementRow(int tid, int64_t int_key,
                                        const char* str_data, uint32_t str_size) {
    IncrementBy(tid, int_key, str_data, str_size, 1);
}

void RadixCount2ColIntStr::IncrementBy(int tid, int64_t int_key,
                                       const char* str_data, uint32_t str_size,
                                       int64_t delta) {
    size_t h2 = ankerl::unordered_dense::hash<std::string_view>{}(
        std::string_view(str_data, str_size));
    size_t h = MixIntStrHash(int_key, h2);
    IncrementByHashed(tid, int_key, str_data, str_size, h, delta);
}

void RadixCount2ColIntStr::IncrementByHashed(int tid, int64_t int_key,
                                             const char* str_data,
                                             uint32_t str_size,
                                             size_t combined_hash,
                                             int64_t delta) {
    auto& pt = *impl_->threads[tid];
    IntStrKey probe{int_key, std::string_view(str_data, str_size), combined_hash};
    int shard = (int)(combined_hash & (IS_NSHARDS - 1));
    auto& m = pt.shards[shard];
    auto it = m.find(probe);
    if (it != m.end()) {
        it->second += delta;
        return;
    }
    char* dst = pt.alloc(str_size);
    if (str_size) std::memcpy(dst, str_data, str_size);
    m.emplace(IntStrKey{int_key, std::string_view(dst, str_size), combined_hash},
              delta);
}

size_t RadixCount2ColIntStr::HashStr(const char* str_data, uint32_t str_size) {
    return ankerl::unordered_dense::hash<std::string_view>{}(
        std::string_view(str_data, str_size));
}

size_t RadixCount2ColIntStr::CombineIntStrHash(int64_t int_key, size_t str_hash) {
    return MixIntStrHash(int_key, str_hash);
}

void RadixCount2ColIntStr::MergeShard(int shard) {
    auto& out = impl_->final_maps[shard];
    size_t max_single = 0;
    for (auto& tl : impl_->threads) {
        size_t sz = tl->shards[shard].size();
        if (sz > max_single) max_single = sz;
    }
    out.reserve(max_single + max_single / 4);
    for (auto& tl : impl_->threads) {
        for (auto& kv : tl->shards[shard]) {
            auto it = out.find(kv.first);
            if (it == out.end()) {
                out.emplace(kv.first, kv.second);
            } else {
                it->second += kv.second;
            }
        }
    }
}

std::vector<RadixCount2ColIntStrResult>
RadixCount2ColIntStr::EmitFirstK(int k) const {
    std::vector<RadixCount2ColIntStrResult> out;
    if (k <= 0) return out;
    out.reserve((size_t)k);
    for (auto& m : impl_->final_maps) {
        for (auto& kv : m) {
            if ((int)out.size() >= k) return out;
            out.push_back({kv.first.i,
                           std::string(kv.first.s),
                           kv.second});
        }
    }
    return out;
}

std::vector<RadixCount2ColIntStrResult>
RadixCount2ColIntStr::EmitTopK(int k) const {
    if (k <= 0) {
        std::vector<RadixCount2ColIntStrResult> all;
        all.reserve(TotalGroups());
        for (auto& m : impl_->final_maps) {
            for (auto& kv : m) {
                all.push_back({kv.first.i,
                               std::string(kv.first.s),
                               kv.second});
            }
        }
        return all;
    }
    auto cmp = [](const RadixCount2ColIntStrResult& a,
                  const RadixCount2ColIntStrResult& b) {
        return a.count > b.count;  // min-heap
    };
    std::priority_queue<RadixCount2ColIntStrResult,
                        std::vector<RadixCount2ColIntStrResult>, decltype(cmp)>
        heap(cmp);
    for (auto& m : impl_->final_maps) {
        for (auto& kv : m) {
            if ((int)heap.size() < k) {
                heap.push({kv.first.i, std::string(kv.first.s), kv.second});
            } else if (kv.second > heap.top().count) {
                heap.pop();
                heap.push({kv.first.i, std::string(kv.first.s), kv.second});
            }
        }
    }
    std::vector<RadixCount2ColIntStrResult> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.push_back(std::move(
            const_cast<RadixCount2ColIntStrResult&>(heap.top())));
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

size_t RadixCount2ColIntStr::TotalGroups() const {
    size_t total = 0;
    for (auto& m : impl_->final_maps) total += m.size();
    return total;
}

size_t RadixCount2ColIntStr::LiveGroupCount() const {
    size_t total = 0;
    for (auto& tl_p : impl_->threads) {
        for (auto& m : tl_p->shards) total += m.size();
    }
    return total;
}

// Q14 Stage 1 inner loop. Per-row IncrementByHashed on (int, str) pairs.
// Specialized for the gstr × int distinct shape: dict path with
// precomputed per-dict-entry str hashes amortizes hashing across rows
// that share the same dict entry.
void RadixCount2ColIntStr::IngestRGStrIntDistinct(int tid,
    const int64_t* int_data_64, const int32_t* int_data_32,
    bool int_is_bigint,
    const uint32_t* dict_indices, const string_t* dict_values,
    uint32_t dict_size, const string_t* str_data,
    const uint8_t* validity_int, const uint8_t* validity_str,
    bool int_all_valid, bool str_all_valid,
    uint32_t nrows, const uint8_t* keep_mask) {
    auto fetch_int = [&](uint32_t r) -> int64_t {
        return int_is_bigint
            ? int_data_64[r] : (int64_t)int_data_32[r];
    };
    if (dict_indices) {
        std::vector<size_t> dict_h(dict_size);
        for (uint32_t d = 0; d < dict_size; d++) {
            dict_h[d] = HashStr(dict_values[d].GetData(),
                                dict_values[d].GetSize());
        }
        // Cache-last (mirrors IngestRGTwoColCount): consecutive rows
        // sharing the same (int, dict_idx) — common on Q14 SearchPhrase
        // batches — skip the per-row hash table find. Stable counter
        // pointer between hits because no insertion happens between them.
        auto& pt = *impl_->threads[tid];
        int64_t prev_k = 0;
        uint32_t prev_d = UINT32_MAX;
        int64_t* prev_cnt_ptr = nullptr;
        for (uint32_t r = 0; r < nrows; r++) {
            if (keep_mask && !keep_mask[r]) continue;
            if (!int_all_valid && !validity_int[r]) continue;
            if (!str_all_valid && !validity_str[r]) continue;
            uint32_t d = dict_indices[r];
            if (d >= dict_size) continue;
            int64_t k = fetch_int(r);
            if (prev_cnt_ptr && k == prev_k && d == prev_d) {
                ++(*prev_cnt_ptr);
                continue;
            }
            size_t ch = CombineIntStrHash(k, dict_h[d]);
            IntStrKey probe{k,
                            std::string_view(dict_values[d].GetData(),
                                             dict_values[d].GetSize()),
                            ch};
            int shard = (int)(ch & (IS_NSHARDS - 1));
            auto& m = pt.shards[shard];
            auto it = m.find(probe);
            if (it != m.end()) {
                ++(it->second);
                prev_cnt_ptr = &it->second;
            } else {
                uint32_t sz = dict_values[d].GetSize();
                char* dst = pt.alloc(sz);
                if (sz) std::memcpy(dst, dict_values[d].GetData(), sz);
                auto ins = m.emplace(IntStrKey{k, std::string_view(dst, sz), ch}, 1);
                prev_cnt_ptr = &ins.first->second;
            }
            prev_k = k;
            prev_d = d;
        }
    } else if (str_data) {
        for (uint32_t r = 0; r < nrows; r++) {
            if (keep_mask && !keep_mask[r]) continue;
            if (!int_all_valid && !validity_int[r]) continue;
            if (!str_all_valid && !validity_str[r]) continue;
            int64_t k = fetch_int(r);
            IncrementRow(tid, k,
                str_data[r].GetData(), str_data[r].GetSize());
        }
    }
}

namespace {
template <typename IntT, bool INT_ALL_VALID, bool STR_ALL_VALID>
inline void StrIntDistinctSkipDiInner(
    RadixCount2ColIntStrImpl& impl, int tid,
    const IntT* int_data,
    const uint32_t* dict_indices, const string_t* dict_values,
    uint32_t dict_size,
    const uint8_t* validity_int, const uint8_t* validity_str,
    const size_t* dict_h,
    uint32_t nrows, uint32_t skip_di) {
    auto& pt = *impl.threads[tid];
    int64_t prev_k = 0;
    uint32_t prev_d = UINT32_MAX;
    int64_t* prev_cnt_ptr = nullptr;
    for (uint32_t r = 0; r < nrows; r++) {
        uint32_t d = dict_indices[r];
        if (d == skip_di) continue;
        if constexpr (!INT_ALL_VALID) { if (!validity_int[r]) continue; }
        if constexpr (!STR_ALL_VALID) { if (!validity_str[r]) continue; }
        if (d >= dict_size) continue;
        int64_t k = (int64_t)int_data[r];
        if (prev_cnt_ptr && k == prev_k && d == prev_d) {
            ++(*prev_cnt_ptr);
            continue;
        }
        size_t ch = MixIntStrHash(k, dict_h[d]);
        IntStrKey probe{k,
                        std::string_view(dict_values[d].GetData(),
                                         dict_values[d].GetSize()),
                        ch};
        int shard = (int)(ch & (IS_NSHARDS - 1));
        auto& m = pt.shards[shard];
        auto it = m.find(probe);
        if (it != m.end()) {
            ++(it->second);
            prev_cnt_ptr = &it->second;
        } else {
            uint32_t sz = dict_values[d].GetSize();
            char* dst = pt.alloc(sz);
            if (sz) std::memcpy(dst, dict_values[d].GetData(), sz);
            auto ins = m.emplace(IntStrKey{k, std::string_view(dst, sz), ch}, 1);
            prev_cnt_ptr = &ins.first->second;
        }
        prev_k = k;
        prev_d = d;
    }
}
}  // namespace

// Skip-di variant: SearchPhrase<>'' folded into dict-idx skip (Q14).
void RadixCount2ColIntStr::IngestRGStrIntDistinctSkipDi(int tid,
    const int64_t* int_data_64, const int32_t* int_data_32,
    bool int_is_bigint,
    const uint32_t* dict_indices, const string_t* dict_values,
    uint32_t dict_size,
    const uint8_t* validity_int, const uint8_t* validity_str,
    bool int_all_valid, bool str_all_valid,
    uint32_t nrows, uint32_t skip_di) {
    if (dict_size == 0 || nrows == 0 || !dict_indices) return;
    std::vector<size_t> dict_h(dict_size);
    for (uint32_t d = 0; d < dict_size; d++) {
        dict_h[d] = HashStr(dict_values[d].GetData(),
                            dict_values[d].GetSize());
    }
#define DISPATCH(T, DATA) \
    if (int_all_valid) { \
        if (str_all_valid) StrIntDistinctSkipDiInner<T, true,  true >(*impl_, tid, DATA, dict_indices, dict_values, dict_size, nullptr, nullptr, dict_h.data(), nrows, skip_di); \
        else               StrIntDistinctSkipDiInner<T, true,  false>(*impl_, tid, DATA, dict_indices, dict_values, dict_size, nullptr, validity_str, dict_h.data(), nrows, skip_di); \
    } else { \
        if (str_all_valid) StrIntDistinctSkipDiInner<T, false, true >(*impl_, tid, DATA, dict_indices, dict_values, dict_size, validity_int, nullptr, dict_h.data(), nrows, skip_di); \
        else               StrIntDistinctSkipDiInner<T, false, false>(*impl_, tid, DATA, dict_indices, dict_values, dict_size, validity_int, validity_str, dict_h.data(), nrows, skip_di); \
    }
    if (int_is_bigint) { DISPATCH(int64_t, int_data_64) }
    else               { DISPATCH(int32_t, int_data_32) }
#undef DISPATCH
}

// Q15-shape inner loop body. Per-RG ingest of (int_key, dict_idx) → count.
// Precomputes per-dict-entry hash once per RG (~10-30 ns saved per row),
// then per-row IncrementByHashed. Lives here (not physical_planner.cpp)
// to keep the planner .text shrink stable across adjacent paths.
//
// Note: a flat-counter dict-amortization variant was attempted (per-RG
// pool[unique_int][dict_size] + fold at end) but didn't move Q15 — the
// pool reallocation + ankerl<int64,u32> int_to_idx overhead masks the
// O(N) → O(unique * touched) insert savings on this shape. Per-row
// IncrementByHashed is the simpler win.
void RadixCount2ColIntStr::IngestRGTwoColCount(int tid,
    const int64_t* int_data_64, const int32_t* int_data_32,
    bool int_is_bigint,
    const uint32_t* dict_indices, const string_t* dict_values,
    uint32_t dict_size,
    const uint8_t* validity_int, const uint8_t* validity_str,
    bool int_all_valid, bool str_all_valid,
    uint32_t nrows, const uint8_t* keep_mask) {
    if (dict_size == 0 || nrows == 0) return;
    thread_local std::vector<size_t> dict_h;
    if (dict_h.size() < dict_size) dict_h.resize(dict_size);
    for (uint32_t d = 0; d < dict_size; d++) {
        dict_h[d] = HashStr(dict_values[d].GetData(),
                            dict_values[d].GetSize());
    }
    auto fetch_int = [&](uint32_t r) -> int64_t {
        return int_is_bigint ? int_data_64[r] : (int64_t)int_data_32[r];
    };
    // Cache-last: ClickBench Q15/Q17 see consecutive rows sharing the
    // same (engine, search_phrase) pair (sorted-ish data — same
    // SearchEngineID + SearchPhrase batched together). Skip the per-row
    // hash + map-find when the pair is unchanged from prior row. The
    // counter pointer stays stable between hits because no insertion
    // happens between them; on miss we re-fetch via IncrementByHashed.
    auto& pt = *impl_->threads[tid];
    int64_t prev_k = 0;
    uint32_t prev_d = UINT32_MAX;
    int64_t* prev_cnt_ptr = nullptr;
    for (uint32_t r = 0; r < nrows; r++) {
        if (keep_mask && !keep_mask[r]) continue;
        if (!int_all_valid && !validity_int[r]) continue;
        if (!str_all_valid && !validity_str[r]) continue;
        uint32_t d = dict_indices[r];
        if (d >= dict_size) continue;
        int64_t k = fetch_int(r);
        if (prev_cnt_ptr && k == prev_k && d == prev_d) {
            ++(*prev_cnt_ptr);
            continue;
        }
        size_t ch = MixIntStrHash(k, dict_h[d]);
        IntStrKey probe{k,
                        std::string_view(dict_values[d].GetData(),
                                         dict_values[d].GetSize()),
                        ch};
        int shard = (int)(ch & (IS_NSHARDS - 1));
        auto& m = pt.shards[shard];
        auto it = m.find(probe);
        if (it != m.end()) {
            ++(it->second);
            prev_cnt_ptr = &it->second;
        } else {
            uint32_t sz = dict_values[d].GetSize();
            char* dst = pt.alloc(sz);
            if (sz) std::memcpy(dst, dict_values[d].GetData(), sz);
            auto ins = m.emplace(IntStrKey{k, std::string_view(dst, sz), ch}, 1);
            prev_cnt_ptr = &ins.first->second;
        }
        prev_k = k;
        prev_d = d;
    }
}

// Same as IngestRGTwoColCount but folds the dict-idx skip in place of a
// keep_mask, skipping the BuildTypedKeepMask + 100MB keep_mask read for the
// SearchPhrase<>'' filter shape. Pass 1's inner loop drops two loads per row
// (no keep_mask + no validity_str when all_valid).
namespace {
template <typename IntT, bool INT_ALL_VALID, bool STR_ALL_VALID>
inline void TwoColCountSkipDiInner(
    RadixCount2ColIntStrImpl& impl, int tid,
    const IntT* int_data,
    const uint32_t* dict_indices, const string_t* dict_values,
    uint32_t dict_size,
    const uint8_t* validity_int, const uint8_t* validity_str,
    const size_t* dict_h,
    uint32_t nrows, uint32_t skip_di) {
    auto& pt = *impl.threads[tid];
    int64_t prev_k = 0;
    uint32_t prev_d = UINT32_MAX;
    int64_t* prev_cnt_ptr = nullptr;
    for (uint32_t r = 0; r < nrows; r++) {
        uint32_t d = dict_indices[r];
        if (d == skip_di) continue;
        if constexpr (!INT_ALL_VALID) { if (!validity_int[r]) continue; }
        if constexpr (!STR_ALL_VALID) { if (!validity_str[r]) continue; }
        if (d >= dict_size) continue;
        int64_t k = (int64_t)int_data[r];
        if (prev_cnt_ptr && k == prev_k && d == prev_d) {
            ++(*prev_cnt_ptr);
            continue;
        }
        size_t ch = MixIntStrHash(k, dict_h[d]);
        IntStrKey probe{k,
                        std::string_view(dict_values[d].GetData(),
                                         dict_values[d].GetSize()),
                        ch};
        int shard = (int)(ch & (IS_NSHARDS - 1));
        auto& m = pt.shards[shard];
        auto it = m.find(probe);
        if (it != m.end()) {
            ++(it->second);
            prev_cnt_ptr = &it->second;
        } else {
            uint32_t sz = dict_values[d].GetSize();
            char* dst = pt.alloc(sz);
            if (sz) std::memcpy(dst, dict_values[d].GetData(), sz);
            auto ins = m.emplace(IntStrKey{k, std::string_view(dst, sz), ch}, 1);
            prev_cnt_ptr = &ins.first->second;
        }
        prev_k = k;
        prev_d = d;
    }
}
}  // namespace

void RadixCount2ColIntStr::IngestRGTwoColCountSkipDi(int tid,
    const int64_t* int_data_64, const int32_t* int_data_32,
    bool int_is_bigint,
    const uint32_t* dict_indices, const string_t* dict_values,
    uint32_t dict_size,
    const uint8_t* validity_int, const uint8_t* validity_str,
    bool int_all_valid, bool str_all_valid,
    uint32_t nrows, uint32_t skip_di) {
    if (dict_size == 0 || nrows == 0) return;
    // thread_local hash cache. dict_size ~30K typical for SearchPhrase RG;
    // per-RG std::vector alloc cost compounds across 95 RGs × 6 threads.
    thread_local std::vector<size_t> dict_h;
    if (dict_h.size() < dict_size) dict_h.resize(dict_size);
    for (uint32_t d = 0; d < dict_size; d++) {
        dict_h[d] = HashStr(dict_values[d].GetData(),
                            dict_values[d].GetSize());
    }
#define DISPATCH(T, DATA) \
    if (int_all_valid) { \
        if (str_all_valid) TwoColCountSkipDiInner<T, true,  true >(*impl_, tid, DATA, dict_indices, dict_values, dict_size, nullptr, nullptr, dict_h.data(), nrows, skip_di); \
        else               TwoColCountSkipDiInner<T, true,  false>(*impl_, tid, DATA, dict_indices, dict_values, dict_size, nullptr, validity_str, dict_h.data(), nrows, skip_di); \
    } else { \
        if (str_all_valid) TwoColCountSkipDiInner<T, false, true >(*impl_, tid, DATA, dict_indices, dict_values, dict_size, validity_int, nullptr, dict_h.data(), nrows, skip_di); \
        else               TwoColCountSkipDiInner<T, false, false>(*impl_, tid, DATA, dict_indices, dict_values, dict_size, validity_int, validity_str, dict_h.data(), nrows, skip_di); \
    }
    if (int_is_bigint) { DISPATCH(int64_t, int_data_64) }
    else               { DISPATCH(int32_t, int_data_32) }
#undef DISPATCH
}

// 2-stage COUNT(DISTINCT INT) GROUP BY VARCHAR: Phase A (parallel)
// builds per-shard local str_count maps from the unique-pair set;
// Phase B reduces sequentially; Phase C heap top-K.
std::vector<RadixCountStrResult>
RadixCount2ColIntStr::EmitTopKDistinctByStrKey(int k) const {
    constexpr int NSHARDS = N_RADIX;
    std::array<ankerl::unordered_dense::map<std::string_view, int64_t>,
               NSHARDS> local_counts;
    auto build_local = [&](int s) {
        auto& out = local_counts[s];
        const auto& m = impl_->final_maps[s];
        out.reserve(m.size() / 2 + 16);
        for (auto& kv : m) {
            ++out[kv.first.s];
        }
    };
    {
        std::vector<std::thread> ts;
        for (int s = 1; s < NSHARDS; s++)
            ts.emplace_back([&, s]() { build_local(s); });
        build_local(0);
        for (auto& t : ts) t.join();
    }
    ankerl::unordered_dense::map<std::string_view, int64_t> str_counts;
    size_t total_local = 0;
    for (auto& m : local_counts) total_local += m.size();
    str_counts.reserve(total_local / 2 + 16);
    for (auto& m : local_counts) {
        for (auto& kv : m) {
            str_counts[kv.first] += kv.second;
        }
    }
    if (k <= 0) {
        std::vector<RadixCountStrResult> all;
        all.reserve(str_counts.size());
        for (auto& kv : str_counts) {
            all.push_back({std::string(kv.first), kv.second});
        }
        return all;
    }
    auto cmp = [](const RadixCountStrResult& a,
                  const RadixCountStrResult& b) {
        return a.count > b.count;  // min-heap on count
    };
    std::priority_queue<RadixCountStrResult,
                        std::vector<RadixCountStrResult>, decltype(cmp)>
        heap(cmp);
    for (auto& kv : str_counts) {
        if ((int)heap.size() < k) {
            heap.push({std::string(kv.first), kv.second});
        } else if (kv.second > heap.top().count) {
            heap.pop();
            heap.push({std::string(kv.first), kv.second});
        }
    }
    std::vector<RadixCountStrResult> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.push_back(std::move(
            const_cast<RadixCountStrResult&>(heap.top())));
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace slothdb

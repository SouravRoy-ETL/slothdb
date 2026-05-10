#include "slothdb/execution/radix_count_agg.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <queue>
#include <string_view>
#include <thread>

#include "third_party/unordered_dense.h"

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
// bump arena for stable string storage. Mirrors q6_string_dedup.
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
        for (uint32_t r = 0; r < nrows; r++) {
            if (keep_mask && !keep_mask[r]) continue;
            if (!int_all_valid && !validity_int[r]) continue;
            if (!str_all_valid && !validity_str[r]) continue;
            uint32_t d = dict_indices[r];
            if (d >= dict_size) continue;
            int64_t k = fetch_int(r);
            size_t ch = CombineIntStrHash(k, dict_h[d]);
            IncrementByHashed(tid, k,
                dict_values[d].GetData(), dict_values[d].GetSize(),
                ch, 1);
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
    std::vector<size_t> dict_h(dict_size);
    for (uint32_t d = 0; d < dict_size; d++) {
        dict_h[d] = HashStr(dict_values[d].GetData(),
                            dict_values[d].GetSize());
    }
    auto fetch_int = [&](uint32_t r) -> int64_t {
        return int_is_bigint ? int_data_64[r] : (int64_t)int_data_32[r];
    };
    for (uint32_t r = 0; r < nrows; r++) {
        if (keep_mask && !keep_mask[r]) continue;
        if (!int_all_valid && !validity_int[r]) continue;
        if (!str_all_valid && !validity_str[r]) continue;
        uint32_t d = dict_indices[r];
        if (d >= dict_size) continue;
        int64_t k = fetch_int(r);
        size_t ch = MixIntStrHash(k, dict_h[d]);
        IncrementByHashed(tid, k,
            dict_values[d].GetData(), dict_values[d].GetSize(),
            ch, 1);
    }
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

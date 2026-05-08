#include "slothdb/execution/radix_multi_agg.hpp"

#include <algorithm>
#include <array>
#include <queue>

#include "third_party/unordered_dense.h"

namespace slothdb {

namespace {

constexpr int N_SHARDS = RadixMultiAggI64Key::N_RADIX;

// Variable-width slot stored AoS in the map. Sized once per agg_count at
// construction. Allocating a vector<int64_t> per slot would be a heap
// alloc per group → death on 5M-group queries; instead we pack count_star
// + 2*num_aggs ints into a single contiguous array shared by sum/cnt.
struct AggSlot {
    int64_t count_star;
    // 2 * num_aggs int64s: [sum0, sum1, ..., cnt0, cnt1, ...]. Tail-allocated
    // via the std::vector below. Fixed-size for the lifetime of the map so
    // no rehashing reallocations needed.
    int64_t* sum_cnt;
};

struct PerThread {
    int num_aggs;
    // Packed slot storage: every slot owns `1 + 2*num_aggs` int64s in this
    // arena. AggSlot.sum_cnt points into it. Vector grows in chunks; we never
    // shrink so pointers stay stable.
    std::array<std::vector<int64_t>, N_SHARDS> arenas;
    std::array<ankerl::unordered_dense::map<uint64_t, uint32_t>, N_SHARDS>
        shards;  // map key -> slot index in arena
    std::array<int64_t, N_SHARDS> count_stars{};  // unused (count_star is in arena)

    void Update(int shard, uint64_t key,
                const int64_t* vals, const uint8_t* valid) {
        auto& m = shards[shard];
        auto& a = arenas[shard];
        size_t per_slot = (size_t)(1 + 2 * num_aggs);
        auto it = m.find(key);
        int64_t* slot;
        if (it == m.end()) {
            uint32_t idx = (uint32_t)(a.size() / per_slot);
            a.resize(a.size() + per_slot, 0);
            slot = a.data() + (size_t)idx * per_slot;
            m.emplace(key, idx);
        } else {
            slot = a.data() + (size_t)it->second * per_slot;
        }
        slot[0]++;  // count_star
        for (int A = 0; A < num_aggs; A++) {
            if (valid[A]) {
                slot[1 + A] += vals[A];
                slot[1 + num_aggs + A]++;
            }
        }
    }
};

}  // anonymous namespace

struct RadixMultiAggImpl {
    int max_threads;
    int num_aggs;
    std::vector<std::unique_ptr<PerThread>> threads;
    // Final per-shard arenas + maps populated by MergeShard.
    std::array<std::vector<int64_t>, N_SHARDS> final_arenas;
    std::array<ankerl::unordered_dense::map<uint64_t, uint32_t>, N_SHARDS>
        final_maps;
};

RadixMultiAggI64Key::RadixMultiAggI64Key(int max_threads, int num_aggs)
    : impl_(std::make_unique<RadixMultiAggImpl>()) {
    impl_->max_threads = max_threads;
    impl_->num_aggs = num_aggs;
    impl_->threads.reserve(max_threads);
    for (int i = 0; i < max_threads; i++) {
        auto pt = std::make_unique<PerThread>();
        pt->num_aggs = num_aggs;
        impl_->threads.push_back(std::move(pt));
    }
}

RadixMultiAggI64Key::~RadixMultiAggI64Key() = default;

int RadixMultiAggI64Key::NumAggs() const { return impl_->num_aggs; }

void RadixMultiAggI64Key::Update(int tid, uint64_t packed_key,
                                  const int64_t* agg_vals,
                                  const uint8_t* agg_valid) {
    auto& pt = *impl_->threads[tid];
    size_t h = ankerl::unordered_dense::hash<uint64_t>{}(packed_key);
    int shard = (int)(h & (N_SHARDS - 1));
    pt.Update(shard, packed_key, agg_vals, agg_valid);
}

void RadixMultiAggI64Key::MergeShard(int shard) {
    auto& out_map = impl_->final_maps[shard];
    auto& out_arena = impl_->final_arenas[shard];
    int num_aggs = impl_->num_aggs;
    size_t per_slot = (size_t)(1 + 2 * num_aggs);
    size_t max_single = 0;
    for (auto& tl : impl_->threads) {
        size_t sz = tl->shards[shard].size();
        if (sz > max_single) max_single = sz;
    }
    out_map.reserve(max_single + max_single / 4);
    out_arena.reserve((max_single + max_single / 4) * per_slot);
    for (auto& tl : impl_->threads) {
        const int64_t* src_arena = tl->arenas[shard].data();
        for (auto& kv : tl->shards[shard]) {
            uint64_t key = kv.first;
            const int64_t* src = src_arena + (size_t)kv.second * per_slot;
            auto it = out_map.find(key);
            int64_t* dst;
            if (it == out_map.end()) {
                uint32_t idx = (uint32_t)(out_arena.size() / per_slot);
                out_arena.resize(out_arena.size() + per_slot, 0);
                dst = out_arena.data() + (size_t)idx * per_slot;
                out_map.emplace(key, idx);
            } else {
                dst = out_arena.data() + (size_t)it->second * per_slot;
            }
            for (size_t i = 0; i < per_slot; i++) dst[i] += src[i];
        }
    }
}

std::vector<RadixMultiAggResult>
RadixMultiAggI64Key::EmitTopK(int k) const {
    int num_aggs = impl_->num_aggs;
    size_t per_slot = (size_t)(1 + 2 * num_aggs);
    auto build_at = [&](uint64_t key, const int64_t* slot,
                        RadixMultiAggResult& r) {
        r.key = key;
        r.count_star = slot[0];
        r.sum.assign(slot + 1, slot + 1 + num_aggs);
        r.cnt.assign(slot + 1 + num_aggs, slot + 1 + 2 * num_aggs);
    };
    if (k <= 0) {
        std::vector<RadixMultiAggResult> all;
        all.reserve(TotalGroups());
        for (int s = 0; s < N_SHARDS; s++) {
            const int64_t* arena = impl_->final_arenas[s].data();
            for (auto& kv : impl_->final_maps[s]) {
                all.emplace_back();
                build_at(kv.first,
                         arena + (size_t)kv.second * per_slot, all.back());
            }
        }
        return all;
    }
    // Cheap heap: (count_star, key, shard, slot_idx). 24 bytes, no
    // per-element vector allocations. Materialise the ≤K winners only at
    // the end. The previous version called build() — allocating two
    // vector<int64_t>s — for every candidate that beat the heap min, which
    // for high-cardinality groups (Q31 ~5.7M) was a hot allocator path.
    struct HeapEntry {
        int64_t count_star;
        uint64_t key;
        uint32_t shard;
        uint32_t slot_idx;
    };
    auto cmp = [](const HeapEntry& a, const HeapEntry& b) {
        return a.count_star > b.count_star;  // min-heap by count_star
    };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)>
        heap(cmp);
    for (int s = 0; s < N_SHARDS; s++) {
        const int64_t* arena = impl_->final_arenas[s].data();
        for (auto& kv : impl_->final_maps[s]) {
            const int64_t* slot = arena + (size_t)kv.second * per_slot;
            int64_t cs = slot[0];
            if ((int)heap.size() < k) {
                heap.push(HeapEntry{cs, kv.first, (uint32_t)s, kv.second});
            } else if (cs > heap.top().count_star) {
                heap.pop();
                heap.push(HeapEntry{cs, kv.first, (uint32_t)s, kv.second});
            }
        }
    }
    std::vector<RadixMultiAggResult> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        const auto& e = heap.top();
        const int64_t* arena = impl_->final_arenas[e.shard].data();
        const int64_t* slot = arena + (size_t)e.slot_idx * per_slot;
        out.emplace_back();
        build_at(e.key, slot, out.back());
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

size_t RadixMultiAggI64Key::TotalGroups() const {
    size_t total = 0;
    for (auto& m : impl_->final_maps) total += m.size();
    return total;
}

// =====================================================================
// RadixMultiAggBigKey — 128-bit composite key variant
// =====================================================================

namespace {

struct BigKey {
    int64_t a;
    int64_t b;
    bool operator==(const BigKey& o) const noexcept {
        return a == o.a && b == o.b;
    }
};

struct BigKeyHash {
    using is_avalanching = void;
    size_t operator()(const BigKey& k) const noexcept {
        size_t h1 = ankerl::unordered_dense::hash<int64_t>{}(k.a);
        size_t h2 = ankerl::unordered_dense::hash<int64_t>{}(k.b);
        return (h1 * 0x9E3779B97F4A7C15ULL) ^ h2;
    }
};

constexpr int BK_NSHARDS = RadixMultiAggBigKey::N_RADIX;

struct BigPerThread {
    int num_aggs;
    std::array<std::vector<int64_t>, BK_NSHARDS> arenas;
    std::array<ankerl::unordered_dense::map<BigKey, uint32_t, BigKeyHash>,
               BK_NSHARDS> shards;

    void Update(int shard, BigKey key,
                const int64_t* vals, const uint8_t* valid) {
        auto& m = shards[shard];
        auto& a = arenas[shard];
        size_t per_slot = (size_t)(1 + 2 * num_aggs);
        auto it = m.find(key);
        int64_t* slot;
        if (it == m.end()) {
            uint32_t idx = (uint32_t)(a.size() / per_slot);
            a.resize(a.size() + per_slot, 0);
            slot = a.data() + (size_t)idx * per_slot;
            m.emplace(key, idx);
        } else {
            slot = a.data() + (size_t)it->second * per_slot;
        }
        slot[0]++;
        for (int A = 0; A < num_aggs; A++) {
            if (valid[A]) {
                slot[1 + A] += vals[A];
                slot[1 + num_aggs + A]++;
            }
        }
    }
};

}  // anonymous namespace

struct RadixMultiAggBigImpl {
    int max_threads;
    int num_aggs;
    std::vector<std::unique_ptr<BigPerThread>> threads;
    std::array<std::vector<int64_t>, BK_NSHARDS> final_arenas;
    std::array<ankerl::unordered_dense::map<BigKey, uint32_t, BigKeyHash>,
               BK_NSHARDS> final_maps;
};

RadixMultiAggBigKey::RadixMultiAggBigKey(int max_threads, int num_aggs)
    : impl_(std::make_unique<RadixMultiAggBigImpl>()) {
    impl_->max_threads = max_threads;
    impl_->num_aggs = num_aggs;
    impl_->threads.reserve(max_threads);
    for (int i = 0; i < max_threads; i++) {
        auto pt = std::make_unique<BigPerThread>();
        pt->num_aggs = num_aggs;
        impl_->threads.push_back(std::move(pt));
    }
}

RadixMultiAggBigKey::~RadixMultiAggBigKey() = default;

int RadixMultiAggBigKey::NumAggs() const { return impl_->num_aggs; }

void RadixMultiAggBigKey::Update(int tid, int64_t key_a, int64_t key_b,
                                  const int64_t* agg_vals,
                                  const uint8_t* agg_valid) {
    auto& pt = *impl_->threads[tid];
    BigKey k{key_a, key_b};
    size_t h = BigKeyHash{}(k);
    int shard = (int)(h & (BK_NSHARDS - 1));
    pt.Update(shard, k, agg_vals, agg_valid);
}

void RadixMultiAggBigKey::MergeShard(int shard) {
    auto& out_map = impl_->final_maps[shard];
    auto& out_arena = impl_->final_arenas[shard];
    int num_aggs = impl_->num_aggs;
    size_t per_slot = (size_t)(1 + 2 * num_aggs);
    size_t max_single = 0;
    for (auto& tl : impl_->threads) {
        size_t sz = tl->shards[shard].size();
        if (sz > max_single) max_single = sz;
    }
    out_map.reserve(max_single + max_single / 4);
    out_arena.reserve((max_single + max_single / 4) * per_slot);
    for (auto& tl : impl_->threads) {
        const int64_t* src_arena = tl->arenas[shard].data();
        for (auto& kv : tl->shards[shard]) {
            BigKey key = kv.first;
            const int64_t* src = src_arena + (size_t)kv.second * per_slot;
            auto it = out_map.find(key);
            int64_t* dst;
            if (it == out_map.end()) {
                uint32_t idx = (uint32_t)(out_arena.size() / per_slot);
                out_arena.resize(out_arena.size() + per_slot, 0);
                dst = out_arena.data() + (size_t)idx * per_slot;
                out_map.emplace(key, idx);
            } else {
                dst = out_arena.data() + (size_t)it->second * per_slot;
            }
            for (size_t i = 0; i < per_slot; i++) dst[i] += src[i];
        }
    }
}

std::vector<RadixMultiAggBigResult>
RadixMultiAggBigKey::EmitTopK(int k) const {
    int num_aggs = impl_->num_aggs;
    size_t per_slot = (size_t)(1 + 2 * num_aggs);
    auto build_at = [&](BigKey key, const int64_t* slot,
                        RadixMultiAggBigResult& r) {
        r.key_a = key.a;
        r.key_b = key.b;
        r.count_star = slot[0];
        r.sum.assign(slot + 1, slot + 1 + num_aggs);
        r.cnt.assign(slot + 1 + num_aggs, slot + 1 + 2 * num_aggs);
    };
    if (k <= 0) {
        std::vector<RadixMultiAggBigResult> all;
        all.reserve(TotalGroups());
        for (int s = 0; s < BK_NSHARDS; s++) {
            const int64_t* arena = impl_->final_arenas[s].data();
            for (auto& kv : impl_->final_maps[s]) {
                all.emplace_back();
                build_at(kv.first,
                         arena + (size_t)kv.second * per_slot, all.back());
            }
        }
        return all;
    }
    // Cheap heap: (count_star, key_a, key_b, shard, slot_idx). 32 bytes, no
    // per-element vector allocations. Materialise the ≤K winners only at
    // the end. Mirrors the I64Key fix.
    struct HeapEntry {
        int64_t count_star;
        int64_t key_a;
        int64_t key_b;
        uint32_t shard;
        uint32_t slot_idx;
    };
    auto cmp = [](const HeapEntry& a, const HeapEntry& b) {
        return a.count_star > b.count_star;
    };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)>
        heap(cmp);
    for (int s = 0; s < BK_NSHARDS; s++) {
        const int64_t* arena = impl_->final_arenas[s].data();
        for (auto& kv : impl_->final_maps[s]) {
            const int64_t* slot = arena + (size_t)kv.second * per_slot;
            int64_t cs = slot[0];
            if ((int)heap.size() < k) {
                heap.push(HeapEntry{cs, kv.first.a, kv.first.b,
                                    (uint32_t)s, kv.second});
            } else if (cs > heap.top().count_star) {
                heap.pop();
                heap.push(HeapEntry{cs, kv.first.a, kv.first.b,
                                    (uint32_t)s, kv.second});
            }
        }
    }
    std::vector<RadixMultiAggBigResult> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        const auto& e = heap.top();
        const int64_t* arena = impl_->final_arenas[e.shard].data();
        const int64_t* slot = arena + (size_t)e.slot_idx * per_slot;
        out.emplace_back();
        build_at(BigKey{e.key_a, e.key_b}, slot, out.back());
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

size_t RadixMultiAggBigKey::TotalGroups() const {
    size_t total = 0;
    for (auto& m : impl_->final_maps) total += m.size();
    return total;
}

}  // namespace slothdb

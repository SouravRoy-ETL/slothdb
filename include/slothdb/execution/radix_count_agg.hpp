#pragma once
// Radix-partitioned COUNT(*) GROUP BY for high-cardinality INT keys.
//
// ClickBench Q16 (`SELECT UserID, COUNT(*) FROM hits GROUP BY UserID
// ORDER BY COUNT(*) DESC LIMIT 10`) has 17.6M unique UserIDs. The default
// FUSED PARQUET path builds one ankerl map per thread, each thread sees
// nearly all unique values → ~12 GB combined → cache-miss every probe →
// 600 ns/row instead of ~30 ns/row.
//
// This path:
//   Phase 1 (per RG, parallel): scatter each row's group key into one of
//     N_RADIX (=16) per-thread buckets, indexed by hash bits.
//   Phase 2 (parallel by radix): each radix worker merges all threads'
//     buckets for its radix into a SimpleI64CountMap. Disjoint by hash
//     so workers don't conflict.
//   Phase 3: TopN heap across the N_RADIX final maps for
//     ORDER BY count DESC LIMIT K. Bypasses the global int_merge_shards
//     emit and the result_rows materialization.
//
// Lives in a separate TU to keep physical_planner.cpp's .text section
// stable (per feedback_text_icache_shift.md and
// feedback_apply_aggs_split_pass_refuted.md).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "slothdb/execution/simple_i64_count_map.hpp"

namespace slothdb {

struct RadixCountResult {
    int64_t key;
    int64_t count;
};

struct RadixCountStrResult {
    std::string key;
    int64_t count;
};

// VARCHAR variant of RadixCountAgg. Per-thread arena holds string copies;
// 16 shards keyed by hash of the string. Phase 2 unions disjoint shards
// across threads. Used for Q13/Q34/Q35-shape queries (single-col VARCHAR
// GROUP BY + only COUNT(*) aggs).
struct RadixCountAggStrImpl;
class RadixCountAggStr {
public:
    static constexpr int N_RADIX = 16;
    explicit RadixCountAggStr(int max_threads);
    ~RadixCountAggStr();
    RadixCountAggStr(const RadixCountAggStr&) = delete;
    RadixCountAggStr& operator=(const RadixCountAggStr&) = delete;

    // Phase 1: per-thread emplace. Hashes string, picks shard, increments
    // count. On miss, copies string into per-thread arena before insert.
    void IncrementRow(int tid, const char* data, uint32_t size);

    // Same as IncrementRow but adds `delta` instead of 1. Used by the
    // per-RG dict-aware path to push N rows worth of count for one dict
    // entry in a single hash + lookup, instead of per-row.
    void IncrementBy(int tid, const char* data, uint32_t size, int64_t delta);

    // Phase 2: parallel per-shard merge. One worker per shard.
    void MergeShard(int shard);

    // Phase 3: top-K (count DESC) across final shards.
    std::vector<RadixCountStrResult> EmitTopK(int k) const;

    size_t TotalGroups() const;

private:
    std::unique_ptr<RadixCountAggStrImpl> impl_;
};

class RadixCountAgg {
public:
    static constexpr int N_RADIX = 16;
    static constexpr int N_RADIX_BITS = 4;

    explicit RadixCountAgg(int max_threads);

    // Phase 1: per-thread scatter. Called once per RG per thread. Hashes
    // the key once, picks one of N_RADIX buckets, appends to that
    // thread's bucket. No hash table operations on the hot path.
    void ScatterRow(int tid, int64_t key);

    // Pre-reserve scatter buffers. Call once with the total expected row
    // count after filter. Reduces vector grow amortization.
    void ReserveExpectedRows(int64_t total_rows);

    // Phase 2: parallel per-radix merge into final maps. One worker per
    // radix. Disjoint buckets → no contention. Iterates each thread's
    // scatter for this radix and inserts into the radix-owned map.
    void MergeRadix(int radix);

    // Phase 3: emit top-K (count DESC). Runs single-threaded over the
    // N_RADIX final maps; each map's top-K candidates feed into a global
    // K-element min-heap. Caller appends (key, count) pairs to result.
    std::vector<RadixCountResult> EmitTopK(int k) const;

    // Returns total unique groups across all radix maps. Used for
    // single-threaded full-emit when LIMIT is large.
    size_t TotalGroups() const;

    // Iterate all (key, count) pairs across all final maps. Used when
    // there's no TopN-LIMIT pushdown and full result is needed.
    template <typename Fn>
    void ForEachGroup(Fn &&fn) const {
        for (auto &m : radix_maps_) {
            m.for_each(fn);
        }
    }

private:
    int max_threads_;
    // Per-thread scatter buffers. tl_scatter_[tid][radix] holds keys.
    std::vector<std::vector<std::vector<int64_t>>> tl_scatter_;
    // Final per-radix count maps. Built in MergeRadix.
    std::vector<SimpleI64CountMap> radix_maps_;
};

}  // namespace slothdb

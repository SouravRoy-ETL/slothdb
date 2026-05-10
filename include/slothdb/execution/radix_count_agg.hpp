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

#include "slothdb/common/types/string_type.hpp"
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

// 2-col composite-key result: (int64, string).
struct RadixCount2ColIntStrResult {
    int64_t int_key;
    std::string str_key;
    int64_t count;
};

// 2-col GROUP BY (INT/BIGINT + VARCHAR) + only COUNT(*) aggs, with optional
// per-RG WHERE filter. ClickBench Q15/Q17/Q18 land here. Composite key is
// (int64, string_view) where the string_view points into a per-thread bump
// arena. Sharded by xor-mix of int hash and string hash so phase 2 merge is
// disjoint by shard, no contention. Mirrors RadixCountAggStr structurally
// but with a composite key + matching emit.
struct RadixCount2ColIntStrImpl;
class RadixCount2ColIntStr {
public:
    static constexpr int N_RADIX = 16;
    explicit RadixCount2ColIntStr(int max_threads);
    ~RadixCount2ColIntStr();
    RadixCount2ColIntStr(const RadixCount2ColIntStr&) = delete;
    RadixCount2ColIntStr& operator=(const RadixCount2ColIntStr&) = delete;

    // Pre-reserve per-thread per-shard maps to avoid resize copies during
    // 99M-row inserts (Q22). Estimate total unique pairs across the agg —
    // we divide by (max_threads * N_RADIX) for per-shard expected count.
    void ReserveExpectedRows(int64_t total_unique_pairs);

    // Phase 1: per-thread emplace. Hashes (int_key, str_data[size]) → shard,
    // increments count. On miss copies string into per-thread arena.
    void IncrementRow(int tid, int64_t int_key,
                      const char* str_data, uint32_t str_size);

    // Same as IncrementRow but adds `delta` instead of 1. Used by per-RG
    // dict-aware path to push N rows worth of count for one (int_key, dict)
    // pair via a single hash + lookup.
    void IncrementBy(int tid, int64_t int_key,
                     const char* str_data, uint32_t str_size, int64_t delta);

    // Same as IncrementRow but with a caller-precomputed combined hash of
    // (int_key, str_data[size]). Caller must use `CombineHash(int_key,
    // str_hash)` (or otherwise produce a well-mixed value) and may cache
    // `HashStr(str_data, str_size)` once per dict entry. Skips the per-row
    // `ankerl::hash<string_view>` so 100M-row hot loops save ~10-15 ns/row
    // on dict-encoded VARCHAR. Q15/Q17 land here.
    void IncrementByHashed(int tid, int64_t int_key,
                           const char* str_data, uint32_t str_size,
                           size_t combined_hash, int64_t delta);

    // Helper to precompute a string hash for caching across rows that share
    // the same dict entry within a row group.
    static size_t HashStr(const char* str_data, uint32_t str_size);
    // Combine an int hash with a string hash into the avalanching key hash
    // used by IncrementByHashed. Matches the internal IntStrKeyHash mixer
    // exactly so per-row callers can reproduce it.
    static size_t CombineIntStrHash(int64_t int_key, size_t str_hash);

    // Phase 2: parallel per-shard merge.
    void MergeShard(int shard);

    // Phase 3: top-K (count DESC) across final shards.
    std::vector<RadixCount2ColIntStrResult> EmitTopK(int k) const;

    // First-K (no ordering, stops as soon as K rows are accumulated).
    // Used by LIMIT-without-ORDER-BY queries (Q22) so we don't pay 80M
    // string copies before LIMIT truncates to 10.
    std::vector<RadixCount2ColIntStrResult> EmitFirstK(int k) const;

    // After Phase 2 merge, treat the (int, str) pairs as the unique-pair
    // set produced by Stage 1 of a 2-stage COUNT(DISTINCT INT) GROUP BY
    // VARCHAR rewrite. Per str_key, count of pairs == COUNT DISTINCT int.
    // Returns top-K (str_key, distinct_int_count) ordered by count DESC.
    // Used by Q14: GROUP BY SearchPhrase + COUNT(DISTINCT UserID).
    std::vector<RadixCountStrResult> EmitTopKDistinctByStrKey(int k) const;

    // Q14 2-stage Stage 1 inner loop body. Lives in this TU (not
    // physical_planner.cpp) so the planner's I-cache footprint stays
    // tight (a +120 LOC inline version regressed Q1/Q3/Q4/Q20 by
    // 15-60% via text-section shift — see I-cache memo).
    // dict_indices is the dict-encoded path; if null, str_data is used.
    // keep_mask is null when no filter is active.
    void IngestRGStrIntDistinct(int tid,
        const int64_t* int_data_64, const int32_t* int_data_32,
        bool int_is_bigint,
        const uint32_t* dict_indices, const string_t* dict_values,
        uint32_t dict_size, const string_t* str_data,
        const uint8_t* validity_int, const uint8_t* validity_str,
        bool int_all_valid, bool str_all_valid,
        uint32_t nrows, const uint8_t* keep_mask);

    // Q15-shape inner loop body: 2-col GROUP BY (INT/BIGINT + VARCHAR-dict)
    // + only COUNT(*) aggs. Mirrors IngestRGStrIntDistinct but increments
    // counts (not building a unique-pair set). Caller already filtered
    // RGs and built keep_mask + validity. Lives here (not in
    // physical_planner.cpp) so the planner's .text shrinks ~30 LOC vs the
    // previous inline loop — keeps Q11/Q12 hot path stable.
    void IngestRGTwoColCount(int tid,
        const int64_t* int_data_64, const int32_t* int_data_32,
        bool int_is_bigint,
        const uint32_t* dict_indices, const string_t* dict_values,
        uint32_t dict_size,
        const uint8_t* validity_int, const uint8_t* validity_str,
        bool int_all_valid, bool str_all_valid,
        uint32_t nrows, const uint8_t* keep_mask);

    size_t TotalGroups() const;

private:
    std::unique_ptr<RadixCount2ColIntStrImpl> impl_;
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

    // Bulk dict-aware ingest for one row group. Two passes:
    //   1) Tight `cnt[di[r]]++` loop over `nrows` (no hash, no map).
    //   2) Once per non-zero `cnt[d]`, one shard.find+emplace with bulk
    //      count. Collapses O(N) per-row map ops to O(D_used) per-RG ops
    //      where D_used <= dict_size << nrows on Q11/Q13 hot paths.
    // `validity` may be null when the column is all-valid; `keep_mask`
    // may be null when no filter is active for the RG. Mirrors DuckDB's
    // GroupedAggregateHashTable::TryAddDictionaryGroups dict-amortization.
    void IncrementByDictRG(int tid,
                           const uint32_t* dict_indices,
                           uint32_t nrows,
                           const string_t* dict_values,
                           uint32_t dict_size,
                           const uint8_t* validity,
                           const uint8_t* keep_mask);

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

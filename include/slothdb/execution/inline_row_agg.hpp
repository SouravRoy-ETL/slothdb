#pragma once
// Unified inline-row hash aggregator. The first stage of replacing the
// per-shape radix_*.cpp family with one templated implementation that
// keeps hash + key + agg state inline in a single contiguous row, after
// DuckDB's GroupedAggregateHashTable + PartitionedTupleData design at
// _private/duckdb/src/execution/aggregate_hashtable.cpp.
//
// Layout per group: Row<NumAggs> = { hash:8, key:8, count_star:8,
// sum[NumAggs]:8 each, cnt[NumAggs]:8 each } = (3 + 2*NumAggs) * 8 bytes.
// For NumAggs <= 2 the row fits in one 64B cache line.
//
// Probe table is a power-of-2 array of ht_entry uint64s after DuckDB's
// ht_entry_t: (salt:16 << 48) | (row_idx + 1). Empty = 0. Linear probing
// with salt prefilter — salt mismatch = no row dereference.
//
// Per-thread radix-sharded build with parallel merge by shard. Replaces
// the I64Key path of radix_multi_agg.cpp (Q31 + variants). Future stages
// extend to BigKey shape, variable-width keys, and VARCHAR keys to
// subsume the entire radix_*.cpp family.

#include <cstdint>
#include <memory>
#include <vector>

namespace slothdb {

template <int NumAggs>
struct InlineRowAggResult {
    uint64_t key;
    int64_t  count_star;
    int64_t  sum[NumAggs];
    int64_t  cnt[NumAggs];
};

template <int NumAggs>
struct InlineRowAggImpl;

template <int NumAggs>
class InlineRowAgg {
public:
    static constexpr int N_RADIX = 16;

    InlineRowAgg(int max_threads);
    ~InlineRowAgg();
    InlineRowAgg(const InlineRowAgg&) = delete;
    InlineRowAgg& operator=(const InlineRowAgg&) = delete;

    // Pre-size per-thread per-shard storage to the expected total unique
    // group count. Each thread × shard gets total / (threads × N_RADIX).
    // Avoids the table-doubling + Row-vector-grow trail when the caller
    // knows the post-filter cardinality upper bound (Q31 ~10M unique
    // pairs after SearchPhrase<>'').
    void ReserveExpectedRows(size_t total_unique_groups);

    // Phase 1: per-thread emplace. Caller passes an already-packed key
    // (typically two INT cols packed into a uint64) plus per-agg input
    // values + null mask. count_star is implicit.
    void Update(int tid, uint64_t packed_key,
                const int64_t* agg_vals,
                const uint8_t* agg_valid);

    // Phase 2: parallel merge. One worker per shard.
    void MergeShard(int shard);

    // Phase 3: TopK by count_star DESC. k <= 0 = full materialise.
    std::vector<InlineRowAggResult<NumAggs>> EmitTopK(int k) const;

    size_t TotalGroups() const;

private:
    std::unique_ptr<InlineRowAggImpl<NumAggs>> impl_;
};

// Stage 1b — composite (int64 a, int64 b) key. For Q32-shape (BIGINT+INT
// or BIGINT+BIGINT) where a uint64 packed key would lose precision. Row
// layout: { hash:8, key_a:8, key_b:8, count_star:8, sum[N]:8N, cnt[N]:8N }
// = (4 + 2N) * 8 bytes. Same probe table + shard architecture as
// InlineRowAgg<N>. SAFETY GATE on caller side: filter required to bound
// cardinality, mirroring RadixMultiAggBigKey's gate (Q33 unguarded
// crashed the test PC at 100M near-unique pairs).
template <int NumAggs>
struct InlineRowAggBigResult {
    int64_t  key_a;
    int64_t  key_b;
    int64_t  count_star;
    int64_t  sum[NumAggs];
    int64_t  cnt[NumAggs];
};

template <int NumAggs>
struct InlineRowAggBigImpl;

template <int NumAggs>
class InlineRowAggBigKey {
public:
    static constexpr int N_RADIX = 16;

    InlineRowAggBigKey(int max_threads);
    ~InlineRowAggBigKey();
    InlineRowAggBigKey(const InlineRowAggBigKey&) = delete;
    InlineRowAggBigKey& operator=(const InlineRowAggBigKey&) = delete;

    // Same purpose as InlineRowAgg::ReserveExpectedRows — pre-size per-
    // thread per-shard storage to avoid table/row growth on high-card
    // 2-col BIGINT GROUP BY (Q32).
    void ReserveExpectedRows(size_t total_unique_groups);

    void Update(int tid, int64_t key_a, int64_t key_b,
                const int64_t* agg_vals,
                const uint8_t* agg_valid);

    void MergeShard(int shard);

    std::vector<InlineRowAggBigResult<NumAggs>> EmitTopK(int k) const;

    size_t TotalGroups() const;

private:
    std::unique_ptr<InlineRowAggBigImpl<NumAggs>> impl_;
};

}  // namespace slothdb

#pragma once
// Radix-partitioned multi-agg GROUP BY for INT+INT keys with up to N aggs of
// type COUNT(*) / SUM / AVG. ClickBench Q31/Q32/Q33-shape:
//   SELECT g0, g1, COUNT(*), SUM(c), AVG(c)
//   FROM hits [WHERE ...] GROUP BY g0, g1 ORDER BY count DESC LIMIT K
//
// Default FUSED PARQUET GENERIC builds one ankerl map per thread keyed by
// the packed-uint64 (g0, g1). For Q31 with ~5M unique pairs per thread the
// map blows past L2/L3 → cache miss every probe → 230 ns/row.
//
// This path:
//   Phase 1 (per RG, parallel): pack the two group cols into uint64, hash
//     once, route per row into per-thread per-shard map. 16 shards keep
//     each working set ~1/16th the size → mostly L2-resident.
//   Phase 2 (parallel by shard): 16 disjoint workers union per-shard maps
//     across threads. No contention.
//   Phase 3: TopK heap on count for ORDER BY count DESC LIMIT K, full
//     materialise otherwise.
//
// Slot stores count_star + N × (sum, count). SUM aggs read .sum[a]; AVG
// reads .sum[a] / .cnt[a]. COUNT(c) reads .cnt[a].
//
// Lives in a separate TU per feedback_text_icache_shift.md.

#include <cstdint>
#include <memory>
#include <vector>

namespace slothdb {

struct RadixMultiAggResult {
    uint64_t key;
    int64_t count_star;
    std::vector<int64_t> sum;   // size = num_aggs
    std::vector<int64_t> cnt;   // size = num_aggs
};

struct RadixMultiAggImpl;
class RadixMultiAggI64Key {
public:
    static constexpr int N_RADIX = 16;
    // num_aggs counts SUM/AVG/COUNT(c) aggs; COUNT(*) is implicit and
    // separate from the array.
    RadixMultiAggI64Key(int max_threads, int num_aggs);
    ~RadixMultiAggI64Key();
    RadixMultiAggI64Key(const RadixMultiAggI64Key&) = delete;
    RadixMultiAggI64Key& operator=(const RadixMultiAggI64Key&) = delete;

    // Phase 1: per-thread emplace. Caller passes parallel arrays of agg
    // values + null mask (validity[a] == 0 means agg input was null and
    // should be skipped). count_star always increments.
    void Update(int tid, uint64_t packed_key,
                const int64_t* agg_vals,
                const uint8_t* agg_valid);

    // Phase 2: parallel per-shard merge. One worker per shard.
    void MergeShard(int shard);

    // Phase 3: TopK by count_star (DESC). k <= 0 returns all groups.
    std::vector<RadixMultiAggResult> EmitTopK(int k) const;

    size_t TotalGroups() const;

    int NumAggs() const;

private:
    std::unique_ptr<RadixMultiAggImpl> impl_;
};

// 128-bit composite-key result: (int64, int64).
struct RadixMultiAggBigResult {
    int64_t key_a;
    int64_t key_b;
    int64_t count_star;
    std::vector<int64_t> sum;
    std::vector<int64_t> cnt;
};

// 2-col GROUP BY where one or both group cols are BIGINT (Q32/Q33-shape:
// WatchID BIGINT + ClientIP INTEGER). 64-bit packed key won't fit so the
// key is a struct {int64 a, int64 b}. Same shard architecture as
// RadixMultiAggI64Key. Slot is contiguous (1 + 2*N_aggs) int64s in a
// per-shard arena, indexed from a small ankerl::map<BigKey, uint32_t>.
struct RadixMultiAggBigImpl;
class RadixMultiAggBigKey {
public:
    static constexpr int N_RADIX = 16;
    RadixMultiAggBigKey(int max_threads, int num_aggs);
    ~RadixMultiAggBigKey();
    RadixMultiAggBigKey(const RadixMultiAggBigKey&) = delete;
    RadixMultiAggBigKey& operator=(const RadixMultiAggBigKey&) = delete;

    // Phase 1: per-thread emplace with composite (a, b) key.
    void Update(int tid, int64_t key_a, int64_t key_b,
                const int64_t* agg_vals, const uint8_t* agg_valid);

    void MergeShard(int shard);
    std::vector<RadixMultiAggBigResult> EmitTopK(int k) const;
    size_t TotalGroups() const;
    int NumAggs() const;

private:
    std::unique_ptr<RadixMultiAggBigImpl> impl_;
};

}  // namespace slothdb

#pragma once

// Specialized aggregator for the Q10 shape: a single INT32-column GROUP BY
// (low cardinality) with aggregates limited to COUNT(*), SUM/AVG of an int
// column, and COUNT(DISTINCT) of an int column.
//
// The generic FUSED-GENERIC per-row per-agg dispatch loop in
// physical_planner.cpp costs ~90 ns/row on this shape (Q10 profile:
// consume ~1.2 s of a ~1.85 s wall). This path hardcodes the agg kinds
// per row group (no per-row branch cascade) and splits the skewed
// distinct-set cross-thread merge across all workers by value hash so one
// mega-group (Q10's busiest RegionID) is not the single-worker long pole.
//
// Lives in a side TU: the in-place column-at-a-time refactor is a known
// I-cache regression for unrelated queries (see
// feedback_apply_aggs_split_pass_refuted.md), and AggState is private to
// physical_planner.cpp so a side-TU consumer keeps its own state.

#include <cstdint>
#include <memory>
#include <vector>

namespace slothdb {

enum class Q10Kind : uint8_t { CountStar, Sum, CountDistinct };

class Q10Agg {
public:
    // `kinds` gives each output aggregate's kind (SUM and AVG are both
    // Sum here - they share the sum+count accumulator and differ only in
    // the caller's emit step).
    Q10Agg(int max_threads, std::vector<Q10Kind> kinds);
    ~Q10Agg();
    Q10Agg(const Q10Agg&) = delete;
    Q10Agg& operator=(const Q10Agg&) = delete;

    // Per-row-group ingest for worker `tid`. `group` is the INT32 group
    // column (nrows entries); `group_valid` may be null (all valid). For
    // each agg a: CountStar ignores the column pointers; Sum and
    // CountDistinct read exactly one of agg_i64[a] / agg_i32[a] (the other
    // is null) and agg_valid[a] (may be null = all valid).
    void ConsumeRG(int tid, uint32_t nrows,
                   const int32_t* group, const uint8_t* group_valid,
                   const std::vector<const int64_t*>& agg_i64,
                   const std::vector<const int32_t*>& agg_i32,
                   const std::vector<const uint8_t*>& agg_valid);

    // One agg's finalized accumulator, mirroring EmitAggView's {count,sum}
    // so the caller emits via the shared EmitAggValue helper:
    //   CountStar     -> count = total rows
    //   Sum           -> sum = running double sum, count = non-null count
    //   CountDistinct -> count = distinct-value count
    struct AggVal { double sum = 0.0; int64_t count = 0; };
    struct Group { int32_t key = 0; std::vector<AggVal> aggs; };

    // Merge every worker's state and return one Group per distinct key.
    // The distinct-set unions are split across workers by value hash.
    std::vector<Group> MergeAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace slothdb

#pragma once

#include "slothdb/common/types/value.hpp"
#include <vector>

namespace slothdb {

class PhysicalOperator;

// Memory-efficient COUNT(*)-only multi-column GROUP BY.
//
// The generic chunk-loop aggregator in physical_planner.cpp stores, per
// group, a std::string key (x3 copies across three maps), a heap-allocated
// std::vector<AggState>, and a std::vector<Value> of typed key values —
// roughly 400+ bytes per group. A GROUP BY with tens of millions of
// distinct groups (e.g. ClickBench Q19, ~56M groups) blows past available
// RAM and either OOMs or thrashes.
//
// When every aggregate in the query is COUNT(*), the only per-group state
// that matters is a single int64 counter. This routine keeps each group as
// a losslessly length-prefixed binary key plus an int64 count (~60-70 bytes
// per group), which fits the same workload in a few GB.
//
// `child` is pulled via GetData(). `group_col_indices` indexes into the
// child's output columns. `num_count_aggs` COUNT(*) result columns are
// appended after the group-key columns (the planner may duplicate an
// aggregate when ORDER BY references its alias).
//
// When `topn_active` is set and `topn_count_order` is true (the query is
// ORDER BY <a COUNT(*) column> with LIMIT), only the `topn_limit` rows with
// the largest (or smallest, per `topn_ascending`) count are materialized.
// This keeps a GROUP BY with tens of millions of groups from materializing
// tens of millions of output rows just for the upstream LIMIT to discard
// nearly all of them. Otherwise every group is emitted.
//
// Returns rows of the form [key cols..., count x num_count_aggs].
std::vector<std::vector<Value>> RunCountGroupAggregate(
    PhysicalOperator *child,
    const std::vector<idx_t> &group_col_indices,
    idx_t num_count_aggs,
    bool topn_active,
    bool topn_count_order,
    bool topn_ascending,
    idx_t topn_limit);

} // namespace slothdb

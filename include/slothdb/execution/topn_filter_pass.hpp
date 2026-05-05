#pragma once

// Filter-aware TopN parquet pass-1 worker. Per-RG: applies a SimplePredicate
// filter via BuildTypedKeepMask, then pushes only matching (key, rg, row)
// tuples into a thread-local heap. Keeps the parallel TopN parquet path
// reachable for queries with FILTER between TopN and SCAN — currently the
// pq_scan unwrap at physical_planner.cpp:1522 only handles PhysicalProjection,
// which forces Q24/Q25/Q26/Q27 onto the slow ExpressionExecutor path
// (Q25 ≈ 60s vs DuckDB 350ms).
//
// Multi-iter project. This header just declares the API; .cpp + integration
// land in subsequent iters.

#include <cstdint>
#include <vector>

#include "slothdb/storage/parquet.hpp"

namespace slothdb {

// Forward declarations to avoid pulling physical_planner.cpp internals.
struct SimplePredicate;

// One filtered candidate row from a row-group's pass-1 scan.
template <typename Key>
struct TopNCandidate {
    Key key;
    uint32_t rg_idx;
    uint32_t row_idx;
    bool is_null;
};

}  // namespace slothdb

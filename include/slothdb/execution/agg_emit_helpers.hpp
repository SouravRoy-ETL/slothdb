#pragma once
// Per-aggregate result emit dispatch for ComputeAggregates.
//
// The 14-branch if/else cascade that converts a finalized AggState into a
// Value used to live inline in physical_planner.cpp. With Q35-shape queries
// producing 5.7 M emit calls, every line in that switch is hot, and any
// edit shifts cache lines for unrelated query paths (Q1/Q7/Q8 sentinels —
// see feedback_text_icache_shift.md and feedback_apply_aggs_split_pass_refuted.md).
//
// Living in a separate TU means future emit-loop optimizations (parallel
// emit, batched dispatch, allocator tweaks) keep physical_planner.cpp's
// .text section stable. The interface uses small PODs so AggState's
// internal layout is not exposed across the TU boundary.

#include "slothdb/common/types/value.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace slothdb {

enum class EmitAggKind : uint8_t {
    Other = 0,
    Count,
    Sum,
    Avg,
    Min,
    Max,
    StringAgg,
    StddevSamp,
    StddevPop,
    VarSamp,
    VarPop,
    Median,
    BoolAnd,
    BoolOr,
};

// Map an uppercased aggregate function name to its emit kind. Names the
// helper does not handle map to Other and the caller must run an inline
// fallback (currently: nothing — the original switch had no default).
EmitAggKind ResolveEmitAggKind(const std::string& upper_name);

struct EmitAggDesc {
    EmitAggKind kind = EmitAggKind::Other;
    LogicalTypeId return_type_id = LogicalTypeId::INVALID;
    bool sum_with_offset = false;
    double sum_offset = 0.0;
};

// View into a single finalized AggState's emit-relevant fields. Built per
// group at emit time. Pointer fields are nullable: pass nullptr when the
// underlying lazy storage (min_val_ptr, max_val_ptr, extras_ptr->values)
// is unset.
struct EmitAggView {
    int64_t count = 0;
    double sum = 0.0;
    double sum_sq = 0.0;
    bool has_min = false;
    double sum_min = 0.0;
    const Value* min_val_ptr = nullptr;
    bool has_max = false;
    double sum_max = 0.0;
    const Value* max_val_ptr = nullptr;
    bool str_started = false;
    const std::string* str_agg = nullptr;
    std::vector<double>* values = nullptr;
    bool bool_and_v = true;
    bool bool_or_v = false;
};

// Append exactly one Value to out_row computed from desc + view. When
// kind is Other nothing is appended and the function returns false; the
// planner historically had no fallback for unknown agg names so the
// result row simply skips that slot.
bool EmitAggValue(const EmitAggDesc& desc, const EmitAggView& view,
                  std::vector<Value>& out_row);

} // namespace slothdb

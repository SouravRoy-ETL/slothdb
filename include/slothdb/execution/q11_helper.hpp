#pragma once

// Q11 / Q12 helper: 1-col VARCHAR GROUP BY + COUNT(DISTINCT INT) hot
// loop. Lives in a side TU per feedback_text_icache_shift.md so the
// inline 50-LOC version doesn't shift physical_planner.cpp .text and
// regress unrelated hot queries (Q21/Q28 demonstrated).

#include <cstdint>
#include <string>
#include <unordered_map>

#include "slothdb/common/types/string_type.hpp"
#include "slothdb/execution/simple_i64_set.hpp"

namespace slothdb {

// gstr × int distinct hot loop, dict_fast variant (Q11 path).
// Per row: keep_row check, validity check, dict_idx fetch, cached
// SimpleI64Set* lookup (di_to_set), agg-int insert. PFD=8 software
// prefetch hides L3 round-trip on per-thread set probes.
void IngestRGGstrIntDistinctDict(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const std::uint32_t* g_dict_idx, const string_t* g_dict_val, std::size_t g_dsz,
    const std::int64_t* a_i64, const std::int32_t* a_i32,
    bool a_all_valid, const std::uint8_t* a_validity,
    const std::uint8_t* keep_mask, bool has_filter,
    std::size_t nrows);

// gstr × int distinct hot loop, plain-string variant (no dict).
void IngestRGGstrIntDistinctPlain(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const string_t* g_str,
    const std::int64_t* a_i64, const std::int32_t* a_i32,
    bool a_all_valid, const std::uint8_t* a_validity,
    const std::uint8_t* keep_mask, bool has_filter,
    std::size_t nrows);

// Same as IngestRGGstrIntDistinctDict, but with the WHERE-filter folded
// into a dict-idx skip: rows where g_dict_idx[r] == skip_di (e.g. the
// empty-string entry for `<col> <> ''`) are dropped without touching the
// 100M-row keep_mask. Saves the BuildTypedKeepMask 100M-row pass plus the
// 100MB keep_mask streaming read, on top of the wasted prefetch on
// filtered rows. Used when the only WHERE predicate is `<group_col> <> ''`
// against the dict-encoded group column.
void IngestRGGstrIntDistinctDictSkipDi(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const std::uint32_t* g_dict_idx, const string_t* g_dict_val, std::size_t g_dsz,
    const std::int64_t* a_i64, const std::int32_t* a_i32,
    bool a_all_valid, const std::uint8_t* a_validity,
    std::uint32_t skip_di,
    std::size_t nrows);

}  // namespace slothdb

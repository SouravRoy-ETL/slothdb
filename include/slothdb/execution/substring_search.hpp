#pragma once

// Helper for COUNT(*) WHERE <varchar> LIKE '%needle%' on a dict-encoded
// VARCHAR column. Fuses the dict-match build + count into a single pass,
// skipping the out_mask materialization (3 passes -> 1 pass) used by the
// generic BuildTypedKeepMask path. Lives in a side TU per
// feedback_text_icache_shift.md — keeps physical_planner.cpp .text shape.

#include <cstddef>
#include <cstdint>
#include <string>

#include "slothdb/common/types/string_type.hpp"

namespace slothdb {

// Returns the per-RG count for COUNT(*) WHERE <varchar> LIKE '%needle%'
// (or NOT LIKE when like_negated) against a dict-encoded VARCHAR column.
//   dict_values / dict_size  : per-RG dictionary
//   dict_indices             : per-row u32 index into dict_values
//   nrows                    : rows in this RG
//   needle / nlen            : bare needle bytes (between the surrounding %s)
//   like_negated             : true for NOT LIKE; false for LIKE
std::int64_t CountDictLikeContains(
    const string_t* dict_values, std::size_t dict_size,
    const std::uint32_t* dict_indices, std::size_t nrows,
    const char* needle, std::size_t nlen,
    bool like_negated);

// PLAIN-encoded fast path. Walks str_data with a needle-size-specialized
// substring search (uint32 prefix compare for 4-7 byte needles, mirroring
// DuckDB's FindStrInStr). Avoids the generic memcmp tail loop that runs
// on every match-candidate in the legacy row-loop. Used for PLAIN-encoded
// row groups, which are the common case for high-cardinality string
// columns.
std::int64_t CountPlainLikeContains(
    const string_t* str_data, std::size_t nrows,
    const char* needle, std::size_t nlen,
    bool like_negated);

}  // namespace slothdb

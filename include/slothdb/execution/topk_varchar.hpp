#pragma once

// Helper for ORDER BY <varchar> ASC/DESC LIMIT N where the output is just
// the order-by column. For a dict-encoded VARCHAR column, the K-smallest
// (or K-largest) strings live in the per-RG dictionary — iterating dict
// entries (~50K per RG) is O(D) instead of O(N) for 100M rows. Filter
// shape supported: single predicate `<key_col> <> ''`.
//
// Lives in side TU per feedback_text_icache_shift.md so planner .text
// stays stable.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "slothdb/common/types/string_type.hpp"

namespace slothdb {

// Output: top-K (smallest if ascending else largest) non-skipped dict
// strings whose dict_idx is referenced at least once by dict_indices.
//
//   dict_values / dict_size : per-RG dict
//   dict_indices / nrows    : per-row dict-idx — used to filter orphan entries
//   validity                : may be null (all valid)
//   skip_di                 : UINT32_MAX = no skip
//   ascending               : true for ASC, false for DESC
//   k                       : top-K count
//
// Returns a vector of (sorted ascending-or-descending) strings, length <= k.
std::vector<std::string> TopKVarcharFromDict(
    const string_t* dict_values, std::size_t dict_size,
    const std::uint32_t* dict_indices, std::size_t nrows,
    const std::uint8_t* validity,
    std::uint32_t skip_di,
    bool ascending, std::size_t k);

// Dict-trust variant: skips per-row dict_indices walk (no orphan check).
// ~10× faster than TopKVarcharFromDict for high-cardinality dicts. Safe
// only when the parquet writer guarantees no orphan dict entries.
std::vector<std::string> TopKVarcharFromDictTrust(
    const string_t* dict_values, std::size_t dict_size,
    std::uint32_t skip_di,
    bool ascending, std::size_t k);

// Used-bitmap variant: same orphan-safety as TopKVarcharFromDict but reads
// the pre-built used[] bitmap supplied by the parquet decoder (str_dict_used).
// Skips the O(N) dict_indices walk — that work moved into the decode batch
// loop where it costs near-zero (used[] writes hit L1 once the bitmap fits).
std::vector<std::string> TopKVarcharFromDictUsed(
    const string_t* dict_values, std::size_t dict_size,
    const std::uint8_t* used,
    std::uint32_t skip_di,
    bool ascending, std::size_t k);

}  // namespace slothdb

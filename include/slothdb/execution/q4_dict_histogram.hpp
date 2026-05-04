#pragma once
// Q4 fast path: dict-histogram SUM/COUNT for BIGINT-dict-encoded columns.
//
// `SELECT AVG(c) FROM t` over an INT64 dict-encoded Parquet column normally
// decodes the column to `i64_data` (an 800MB sequential write across 100M
// rows on hits.parquet's UserID) and then sums it. For dict-encoded columns
// with bit_width <= ~16, the dict is small (~31k entries per RG) and most
// rows reference a fraction of the dict. We can:
//   1. Per RG, build a histogram count[idx] over the RLE-decoded indices
//      (single pass, ~442k 4-byte writes into a ~120KB array — L2-resident).
//   2. Reduce sum_local = sum_idx (count[idx] * dict.i64[idx]) — ~31k mul-add.
// This avoids materializing i64_data entirely (the 800MB sequential write
// dominates Q4 wall per the q4_profile memo).
//
// Lives in a separate TU to keep physical_planner.cpp's .text section
// stable across this change (see feedback_text_icache_shift.md).

#include "slothdb/common/constants.hpp"
#include <cstdint>

namespace slothdb {

class ParquetReader;

// Drive a parallel per-RG histogram-decode for an INT64 dict-encoded column.
// On success, sets out_count to the number of non-null rows seen and
// out_sum to the SUM as a double (matches the existing AggState.sum/count
// semantics — the calling planner converts to AVG by sum/count).
//
// Returns false if any RG of the column is NOT BIGINT-dict-encodable
// (e.g. has PLAIN data pages or non-INT64 type) — caller must fall back
// to the standard fused parquet aggregate path.
bool TryQ4DictHistogram(ParquetReader &reader, idx_t col_idx,
                        int64_t &out_count, double &out_sum);

} // namespace slothdb

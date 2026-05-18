#pragma once
// Fast path: dict-histogram SUM/COUNT for BIGINT-dict-encoded columns.
//
// `SELECT AVG(c) FROM t` over an INT64 dict-encoded Parquet column normally
// decodes the column to `i64_data` (a large sequential write — hundreds of
// MB across a 100M-row column) and then sums it. For dict-encoded columns
// with a small bit_width, the dict is small (~tens of thousands of entries
// per RG) and most rows reference a fraction of the dict. We can:
//   1. Per RG, build a histogram count[idx] over the RLE-decoded indices
//      (single pass, ~hundreds of K 4-byte writes into an L2-resident array).
//   2. Reduce sum_local = sum_idx (count[idx] * dict.i64[idx]) — one mul-add
//      per dict entry.
// This avoids materializing i64_data entirely (the large sequential write
// dominates the wall time of an ungrouped SUM/AVG over a wide INT64 column).
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
bool TryDictHistogramAgg(ParquetReader &reader, idx_t col_idx,
                         int64_t &out_count, double &out_sum);

} // namespace slothdb

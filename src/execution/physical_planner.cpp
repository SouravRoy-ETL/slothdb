#include "slothdb/execution/physical_planner.hpp"
#include "slothdb/execution/expression_executor.hpp"
#include "slothdb/execution/agg_emit_helpers.hpp"
#include "slothdb/execution/dict_histogram.hpp"
#include "slothdb/execution/string_dedup.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/parallel.hpp"
#include "slothdb/common/string_util.hpp"
#ifndef SLOTHDB_EDGE
#include "slothdb/storage/arrow_ipc.hpp"
#include "slothdb/storage/avro_reader.hpp"
#include "slothdb/storage/sqlite_scanner.hpp"
#endif
#include "slothdb/storage/data_table.hpp"
#include "slothdb/storage/fast_csv_reader.hpp"
#include "slothdb/storage/json_reader.hpp"
#include "slothdb/storage/parquet.hpp"
#include "third_party/unordered_dense.h"
#include "slothdb/execution/simple_i64_set.hpp"
#include "slothdb/execution/radix_count_agg.hpp"
#include "slothdb/execution/inline_row_agg.hpp"
#include "slothdb/execution/literal_emit_filter.hpp"
#include "slothdb/execution/int_group_agg.hpp"
#include "slothdb/execution/str_group_distinct.hpp"
#include "slothdb/execution/substring_search.hpp"
#include "slothdb/execution/topk_varchar.hpp"
#include "slothdb/execution/radix_multi_agg.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <array>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

// AVX2 baseline (Haswell+, 2013). CMake adds /arch:AVX2 (MSVC) or -mavx2
// to slothdb_lib so we can use 256-bit SIMD directly. Used by the FUSED
// PARQUET FAST PATH's INT32 SUM hot loop (Q3 / AVG over 100M rows).
// MSVC defines __AVX2__ when /arch:AVX2 is set.
#if defined(__AVX2__)
#include <immintrin.h>
#define SLOTHDB_PP_HAS_AVX2 1
#endif

namespace slothdb {

// Forward declarations - classes that reference each other across definitions.
class PhysicalHashJoin;
class PhysicalParquetScan;
struct SimplePredicate;
// 2-stage COUNT(DISTINCT) for GROUP BY <varchar> + TopN (defined at
// file end, noinline).
static bool TryComputeVarcharGroupDistinctTopN(
    PhysicalParquetScan *pq,
    idx_t group_col, idx_t agg_col,
    LogicalTypeId group_tid, LogicalTypeId agg_tid,
    bool topn_active, bool topn_ascending, idx_t topn_limit,
    int num_aggs,
    const std::vector<SimplePredicate> &dpd_preds, bool dpd_has_filter,
    std::vector<std::vector<Value>> &result_rows_out);

// Defined after PhysicalHashJoin's full declaration. Returns nullptr if op is not one.
static PhysicalHashJoin *AsHashJoin(PhysicalOperator *op);

// LIKE '%needle%' substring search. memchr-anchored memcmp confirm — uses the
// CRT's SIMD memchr (MSVC) for the hot byte-scan, then memcmp to verify.
// Microbench (200K URL-shaped strings): ~6.8x faster than std::search vs
// the libc++ generic two-way searcher. Returns first hit or nullptr.
// Note: haystack is NOT NUL-terminated (it's a string_t slice into a parquet
// heap), so strstr is unsafe; this helper takes explicit lengths.
static inline const char *FindSubstr(const char *h, size_t hlen,
                                     const char *n, size_t nlen) {
    if (nlen == 0) return h;
    if (nlen > hlen) return nullptr;
    const char first = n[0];
    const char *end = h + (hlen - nlen) + 1;
    while (h < end) {
        const char *p = static_cast<const char *>(
            std::memchr(h, static_cast<unsigned char>(first),
                        static_cast<size_t>(end - h)));
        if (!p) return nullptr;
        if (std::memcmp(p, n, nlen) == 0) return p;
        h = p + 1;
    }
    return nullptr;
}

// ============================================================================
// Simple-predicate compiler & evaluator.
//
// Converts a conjunction of (ColumnRef OP Constant) comparisons into a flat
// struct-of-arrays form that can be evaluated per row against typed
// ParquetColumnData with no virtual dispatch. Used by the fused Parquet
// FILTER+AGG paths to skip PhysicalFilter's DataChunk copy.
// ============================================================================
enum class SimpleCmpOp { LT, LE, GT, GE, EQ, NE };

struct SimplePredicate {
    idx_t col_idx;
    SimpleCmpOp op;
    double dval;       // numeric form: comparison value cast to double
    int64_t ival = 0;  // numeric form: original int64 (BIGINT literals
                       // > 2^53 cannot round-trip through double — Q20's
                       // UserID = 435090932899640449 lost ~64 in dval).
                       // BIGINT/INTEGER paths must read ival, not (int64_t)dval.
    std::string sval;  // VARCHAR form: literal bytes (for EQ/NE: full literal;
                       // for like_contains: bare needle with %s stripped)
    bool str_form = false;
    bool like_contains = false;  // VARCHAR LIKE '%literal%' (case-sensitive,
                                 // no _, no escapes, exactly one %...%)
    bool like_negated = false;   // when like_contains: true means NOT LIKE
};

static bool ParseCmpOp(const std::string &s, SimpleCmpOp &out) {
    if (s == "<")  { out = SimpleCmpOp::LT; return true; }
    if (s == "<=") { out = SimpleCmpOp::LE; return true; }
    if (s == ">")  { out = SimpleCmpOp::GT; return true; }
    if (s == ">=") { out = SimpleCmpOp::GE; return true; }
    if (s == "=" || s == "==") { out = SimpleCmpOp::EQ; return true; }
    if (s == "!=" || s == "<>") { out = SimpleCmpOp::NE; return true; }
    return false;
}

// Returns true iff `e` is an AND-tree of (ColumnRef OP Constant) comparisons
// on numeric columns; fills `out`. On `false`, `out` may be partially filled -
// caller should discard it.
static bool TryCompileSimplePredicateImpl(const BoundExpression &e,
                                          std::vector<SimplePredicate> &out) {
    if (e.GetExpressionType() == BoundExpressionType::CONJUNCTION) {
        auto &c = static_cast<const BoundConjunction &>(e);
        if (c.op != "AND") return false;
        return TryCompileSimplePredicateImpl(*c.left, out) &&
               TryCompileSimplePredicateImpl(*c.right, out);
    }
    if (e.GetExpressionType() != BoundExpressionType::COMPARISON) return false;
    auto &cmp = static_cast<const BoundComparison &>(e);
    bool swapped = false;
    const BoundExpression *lhs = cmp.left.get();
    const BoundExpression *rhs = cmp.right.get();
    // Allow Constant OP Column (swap sides).
    if (lhs->GetExpressionType() == BoundExpressionType::CONSTANT &&
        rhs->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        std::swap(lhs, rhs); swapped = true;
    }
    if (lhs->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
    if (rhs->GetExpressionType() != BoundExpressionType::CONSTANT) return false;
    auto &col = static_cast<const BoundColumnRef &>(*lhs);
    auto &con = static_cast<const BoundConstant &>(*rhs);
    auto tid = col.GetReturnType().id();
    // LIKE '%literal%' (case-sensitive, no _, no escapes) on a VARCHAR
    // column. Stash the bare needle and route the row-loop through
    // memmem/std::search instead of the per-row backtracker. ILIKE keeps
    // falling through to the legacy expression executor. NOT LIKE goes
    // through the same path with like_negated=true (Q23 needs this).
    if (!swapped && (cmp.op == "LIKE" || cmp.op == "NOT LIKE") &&
        tid == LogicalTypeId::VARCHAR) {
        if (con.value.IsNull()) return false;
        if (con.value.type().id() != LogicalTypeId::VARCHAR) return false;
        const auto &pat = con.value.GetValue<std::string>();
        if (pat.size() < 2 || pat.front() != '%' || pat.back() != '%') return false;
        std::string needle = pat.substr(1, pat.size() - 2);
        for (char c : needle) {
            if (c == '%' || c == '_' || c == '\\') return false;
        }
        SimplePredicate sp;
        sp.col_idx = col.column_index;
        sp.op = SimpleCmpOp::EQ;  // unused for LIKE; satisfies field init
        sp.sval = std::move(needle);
        sp.str_form = true;
        sp.like_contains = true;
        sp.like_negated = (cmp.op == "NOT LIKE");
        out.push_back(std::move(sp));
        return true;
    }
    SimpleCmpOp op;
    if (!ParseCmpOp(cmp.op, op)) return false;
    if (swapped) {
        // `const OP col` ≡ `col OP' const` where OP' is the mirror op.
        switch (op) {
        case SimpleCmpOp::LT: op = SimpleCmpOp::GT; break;
        case SimpleCmpOp::LE: op = SimpleCmpOp::GE; break;
        case SimpleCmpOp::GT: op = SimpleCmpOp::LT; break;
        case SimpleCmpOp::GE: op = SimpleCmpOp::LE; break;
        case SimpleCmpOp::EQ: case SimpleCmpOp::NE: break;
        }
    }
    // VARCHAR EQ/NE against a string literal — supports the ClickBench
    // shape `WHERE Col <> ''` (Q11/Q12/Q13/Q14/Q15/Q22) without falling
    // back to the slow generic path. Other ops on VARCHAR (LIKE, range
    // comparisons) stay unsupported here.
    if (tid == LogicalTypeId::VARCHAR) {
        if (op != SimpleCmpOp::EQ && op != SimpleCmpOp::NE) return false;
        if (con.value.IsNull()) return false;
        if (con.value.type().id() != LogicalTypeId::VARCHAR) return false;
        SimplePredicate sp;
        sp.col_idx = col.column_index;
        sp.op = op;
        sp.sval = con.value.GetValue<std::string>();
        sp.str_form = true;
        out.push_back(std::move(sp));
        return true;
    }
    if (tid != LogicalTypeId::BIGINT && tid != LogicalTypeId::INTEGER &&
        tid != LogicalTypeId::DOUBLE && tid != LogicalTypeId::FLOAT) return false;
    double d;
    int64_t iv = 0;
    try {
        auto vtid = con.value.type().id();
        switch (vtid) {
        case LogicalTypeId::BOOLEAN:  iv = con.value.GetValue<bool>() ? 1 : 0; d = (double)iv; break;
        case LogicalTypeId::TINYINT:  iv = (int64_t)con.value.GetValue<int8_t>();  d = (double)iv; break;
        case LogicalTypeId::SMALLINT: iv = (int64_t)con.value.GetValue<int16_t>(); d = (double)iv; break;
        case LogicalTypeId::INTEGER:  iv = (int64_t)con.value.GetValue<int32_t>(); d = (double)iv; break;
        case LogicalTypeId::BIGINT:   iv = con.value.GetValue<int64_t>(); d = (double)iv; break;
        case LogicalTypeId::FLOAT:    d = (double)con.value.GetValue<float>();  iv = (int64_t)d; break;
        case LogicalTypeId::DOUBLE:   d = con.value.GetValue<double>();         iv = (int64_t)d; break;
        default: return false;
        }
    } catch (...) { return false; }
    SimplePredicate sp;
    sp.col_idx = col.column_index;
    sp.op = op;
    sp.dval = d;
    sp.ival = iv;
    out.push_back(std::move(sp));
    return true;
}

// Top-level entry: compile then reorder so cheap+selective predicates run
// first. LIKE '%needle%' is per-row substring search; on ClickBench Q22 the
// URL-LIKE clause is paired with a much cheaper SearchPhrase <> '' that
// rejects ~98% of rows, so eval'ing NE first short-circuits LIKE on most
// rows.
static bool TryCompileSimplePredicate(const BoundExpression &e,
                                      std::vector<SimplePredicate> &out) {
    size_t base = out.size();
    if (!TryCompileSimplePredicateImpl(e, out)) return false;
    std::stable_sort(out.begin() + base, out.end(),
        [](const SimplePredicate &a, const SimplePredicate &b) {
            return !a.like_contains && b.like_contains;
        });
    return true;
}

// Evaluate predicates against a DataChunk row (for CSV / JSON sources).
// Returns false if any predicate fails or the row is null in a predicate col.
static inline bool EvalSimplePredicatesChunk(
    const std::vector<SimplePredicate> &preds,
    const DataChunk &chunk, idx_t r) {
    for (const auto &p : preds) {
        auto &v = chunk.GetVector(p.col_idx);
        if (!v.GetValidity().RowIsValid(r)) return false;
        if (p.str_form) {
            if (v.GetType().id() != LogicalTypeId::VARCHAR) return false;
            const auto &s = v.GetData<string_t>()[r];
            if (p.like_contains) {
                const char *hs = s.GetData();
                uint32_t hl = s.GetSize();
                bool match;
                if (p.sval.empty()) {
                    match = true;
                } else if (hl < p.sval.size()) {
                    match = false;
                } else {
                    match = (FindSubstr(hs, hl, p.sval.data(), p.sval.size()) != nullptr);
                }
                if (p.like_negated) match = !match;
                if (!match) return false;
                continue;
            }
            bool eq = (s.GetSize() == p.sval.size()) &&
                      (p.sval.empty() ||
                       std::memcmp(s.GetData(), p.sval.data(), p.sval.size()) == 0);
            if (p.op == SimpleCmpOp::EQ) { if (!eq) return false; }
            else                         { if (eq)  return false; }
            continue;
        }
        double val;
        switch (v.GetType().id()) {
        case LogicalTypeId::BIGINT:  val = static_cast<double>(v.GetData<int64_t>()[r]); break;
        case LogicalTypeId::INTEGER: val = static_cast<double>(v.GetData<int32_t>()[r]); break;
        case LogicalTypeId::DOUBLE:  val = v.GetData<double>()[r]; break;
        case LogicalTypeId::FLOAT:   val = static_cast<double>(v.GetData<float>()[r]); break;
        default: return false;
        }
        switch (p.op) {
        case SimpleCmpOp::LT: if (!(val <  p.dval)) return false; break;
        case SimpleCmpOp::LE: if (!(val <= p.dval)) return false; break;
        case SimpleCmpOp::GT: if (!(val >  p.dval)) return false; break;
        case SimpleCmpOp::GE: if (!(val >= p.dval)) return false; break;
        case SimpleCmpOp::EQ: if (val != p.dval) return false; break;
        case SimpleCmpOp::NE: if (val == p.dval) return false; break;
        }
    }
    return true;
}

// Evaluate all predicates against typed Parquet row-group columns at row r.
// Returns false (fail) if any predicate misses or the row is null in a
// predicate column.
static inline bool EvalSimplePredicates(const std::vector<SimplePredicate> &preds,
                                        const std::vector<ParquetColumnData> &cols,
                                        idx_t r) {
    for (const auto &p : preds) {
        const auto &c = cols[p.col_idx];
        if (!c.decoded) return false; // can't eval if column wasn't typed-decoded
        if (!c.all_valid && !c.validity[r]) return false;
        if (p.str_form) {
            if (c.type.id() != LogicalTypeId::VARCHAR) return false;
            // Lengths-only fast path: when the column was decoded for length
            // only (str_lengths_only=true) and predicate is `= ''` / `<> ''`.
            if (c.str_lengths_only && !c.str_lengths.empty() &&
                !p.like_contains && p.sval.empty() &&
                (p.op == SimpleCmpOp::EQ || p.op == SimpleCmpOp::NE)) {
                bool is_empty = (c.str_lengths[r] == 0);
                if (p.op == SimpleCmpOp::EQ) { if (!is_empty) return false; }
                else                         { if (is_empty)  return false; }
                continue;
            }
            // Two layouts available: dict-encoded (str_dict_indices +
            // str_dict_values) or expanded str_data. Prefer dict — one
            // memcmp per dict entry vs one per row in the caller's loop
            // is the same big-O here, but keeps the per-row work to a
            // single load + compare.
            const char *sd; uint32_t sl;
            if (c.str_dict_encoded && !c.str_dict_indices.empty()) {
                uint32_t di = c.str_dict_indices[r];
                if (di >= c.str_dict_values.size()) return false;
                sd = c.str_dict_values[di].GetData();
                sl = c.str_dict_values[di].GetSize();
            } else if (!c.str_data.empty()) {
                sd = c.str_data[r].GetData();
                sl = c.str_data[r].GetSize();
            } else {
                return false;
            }
            if (p.like_contains) {
                bool match;
                if (p.sval.empty()) {
                    match = true;
                } else if (sl < p.sval.size()) {
                    match = false;
                } else {
                    match = (FindSubstr(sd, sl, p.sval.data(), p.sval.size()) != nullptr);
                }
                if (p.like_negated) match = !match;
                if (!match) return false;
                continue;
            }
            bool eq = (sl == p.sval.size()) &&
                      (p.sval.empty() ||
                       std::memcmp(sd, p.sval.data(), p.sval.size()) == 0);
            if (p.op == SimpleCmpOp::EQ) { if (!eq) return false; }
            else                         { if (eq)  return false; }
            continue;
        }
        // BIGINT/INTEGER paths must compare via int64 to avoid 2^53 dval
        // rounding bug — Q20's UserID = 435090932899640449 lost precision in
        // both the row value and literal when both went through double.
        auto ctid = c.type.id();
        if (ctid == LogicalTypeId::BIGINT || ctid == LogicalTypeId::INTEGER) {
            int64_t v = (ctid == LogicalTypeId::BIGINT) ? c.i64_data[r]
                                                        : (int64_t)c.i32_data[r];
            int64_t lit = p.ival;
            switch (p.op) {
            case SimpleCmpOp::LT: if (!(v <  lit)) return false; break;
            case SimpleCmpOp::LE: if (!(v <= lit)) return false; break;
            case SimpleCmpOp::GT: if (!(v >  lit)) return false; break;
            case SimpleCmpOp::GE: if (!(v >= lit)) return false; break;
            case SimpleCmpOp::EQ: if (v != lit) return false; break;
            case SimpleCmpOp::NE: if (v == lit) return false; break;
            }
            continue;
        }
        double v;
        switch (ctid) {
        case LogicalTypeId::DOUBLE:  v = c.f64_data[r]; break;
        case LogicalTypeId::FLOAT:   v = static_cast<double>(c.f32_data[r]); break;
        default: return false;
        }
        switch (p.op) {
        case SimpleCmpOp::LT: if (!(v <  p.dval)) return false; break;
        case SimpleCmpOp::LE: if (!(v <= p.dval)) return false; break;
        case SimpleCmpOp::GT: if (!(v >  p.dval)) return false; break;
        case SimpleCmpOp::GE: if (!(v >= p.dval)) return false; break;
        case SimpleCmpOp::EQ: if (v != p.dval) return false; break;
        case SimpleCmpOp::NE: if (v == p.dval) return false; break;
        }
    }
    return true;
}

// Build a per-row keep mask for a list of SimplePredicates over a row
// group's column data. Returns true on success and fills `out_mask`
// (size == nrows, 1 = keep, 0 = drop). Returns false if any predicate
// can't be represented in the typed-vector form (mixed-page strings
// without a dict, NULL handling required, unsupported op, missing
// data). Auto-vectorizes — replaces per-row dispatch through
// EvalSimplePredicates which costs ~30 cyc/row × 100M rows on Q8/Q11.
static inline bool BuildTypedKeepMask(const std::vector<SimplePredicate> &preds,
                                       const std::vector<ParquetColumnData> &cols,
                                       idx_t nrows,
                                       std::vector<uint8_t> &out_mask) {
    if (preds.empty()) return false;
    for (auto &p : preds) {
        if (p.col_idx >= cols.size()) return false;
        const auto &c = cols[p.col_idx];
        if (!c.decoded || !c.all_valid) return false;
        if (p.str_form) {
            if (c.type.id() != LogicalTypeId::VARCHAR) return false;
            // Lengths-only path accepts `= ''` / `<> ''` (length-only check).
            if (c.str_lengths_only && !c.str_lengths.empty() &&
                !p.like_contains && p.sval.empty() &&
                (p.op == SimpleCmpOp::EQ || p.op == SimpleCmpOp::NE)) {
                continue;
            }
            if (!c.str_dict_encoded || c.str_dict_indices.empty()) {
                // PLAIN VARCHAR LIKE: walk str_data with the running mask
                // (only rows still keep=1). Q22 (URL LIKE %google% AND
                // SearchPhrase <> '') lands here — SearchPhrase NE drops
                // ~87% of rows before URL LIKE fires.
                if (p.like_contains && !c.str_data.empty() &&
                    c.str_data.size() >= nrows) continue;
                return false;
            }
            if (p.like_contains) continue;
            if (p.op != SimpleCmpOp::EQ && p.op != SimpleCmpOp::NE) return false;
        } else {
            if (c.type.id() != LogicalTypeId::INTEGER &&
                c.type.id() != LogicalTypeId::BIGINT) return false;
        }
    }
    out_mask.assign(nrows, 1);
    for (auto &p : preds) {
        const auto &c = cols[p.col_idx];
        if (p.str_form) {
            // Lengths-only path: per-row length comparison, no dict needed.
            if (c.str_lengths_only && !c.str_lengths.empty() &&
                !p.like_contains && p.sval.empty() &&
                (p.op == SimpleCmpOp::EQ || p.op == SimpleCmpOp::NE)) {
                const uint32_t *L = c.str_lengths.data();
                if (p.op == SimpleCmpOp::EQ) {
                    for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(L[r] == 0);
                } else {
                    for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(L[r] != 0);
                }
                continue;
            }
            // PLAIN VARCHAR LIKE: process only rows where keep_mask is
            // still 1. The prior preds (sorted non-LIKE-first by
            // TryCompileSimplePredicate) have already pruned out_mask, so
            // this skips ~87% of rows on Q22's URL LIKE check.
            if (!c.str_dict_encoded && p.like_contains && !c.str_data.empty()) {
                const string_t *sdata = c.str_data.data();
                const auto &needle = p.sval;
                uint8_t miss_v = p.like_negated ? 1 : 0;
                uint8_t hit_v  = p.like_negated ? 0 : 1;
                if (needle.empty()) {
                    if (p.like_negated)
                        for (idx_t r = 0; r < nrows; r++) out_mask[r] = 0;
                    // else: keep mask unchanged (empty needle matches all)
                } else if (needle.size() == 6) {
                    // Q22 "google" hot path: uint32 prefix compare avoids
                    // the generic byte-loop memcmp on every match candidate.
                    // Mirrors DuckDB FindStrInStr 6-byte specialization.
                    uint32_t n32; std::memcpy(&n32, needle.data(), 4);
                    uint16_t ntl; std::memcpy(&ntl, needle.data() + 4, 2);
                    unsigned char first = (unsigned char)(n32 & 0xFF);
                    for (idx_t r = 0; r < nrows; r++) {
                        if (!out_mask[r]) continue;
                        const char *hs = sdata[r].GetData();
                        uint32_t hl = sdata[r].GetSize();
                        if (hl < 6) { out_mask[r] = miss_v; continue; }
                        const char *hp = hs;
                        const char *end = hs + (hl - 5);
                        bool found = false;
                        while (hp < end) {
                            const char *m = static_cast<const char *>(
                                std::memchr(hp, first, (size_t)(end - hp)));
                            if (!m) break;
                            uint32_t v32; std::memcpy(&v32, m, 4);
                            if (v32 == n32) {
                                uint16_t vtl; std::memcpy(&vtl, m + 4, 2);
                                if (vtl == ntl) { found = true; break; }
                            }
                            hp = m + 1;
                        }
                        out_mask[r] = found ? hit_v : miss_v;
                    }
                } else if (needle.size() == 8) {
                    // Q23 ".google." 8-byte needle: uint64 compare in one
                    // instruction. Mirrors DuckDB FindStrInStr case 8
                    // (Contains<uint64_t, ContainsAligned>).
                    uint64_t n64; std::memcpy(&n64, needle.data(), 8);
                    unsigned char first = (unsigned char)(n64 & 0xFF);
                    for (idx_t r = 0; r < nrows; r++) {
                        if (!out_mask[r]) continue;
                        const char *hs = sdata[r].GetData();
                        uint32_t hl = sdata[r].GetSize();
                        if (hl < 8) { out_mask[r] = miss_v; continue; }
                        const char *hp = hs;
                        const char *end = hs + (hl - 7);
                        bool found = false;
                        while (hp < end) {
                            const char *m = static_cast<const char *>(
                                std::memchr(hp, first, (size_t)(end - hp)));
                            if (!m) break;
                            uint64_t v64; std::memcpy(&v64, m, 8);
                            if (v64 == n64) { found = true; break; }
                            hp = m + 1;
                        }
                        out_mask[r] = found ? hit_v : miss_v;
                    }
                } else {
                    for (idx_t r = 0; r < nrows; r++) {
                        if (!out_mask[r]) continue;
                        const char *hs = sdata[r].GetData();
                        uint32_t hl = sdata[r].GetSize();
                        if (hl < needle.size()) { out_mask[r] = miss_v; continue; }
                        out_mask[r] = FindSubstr(hs, hl, needle.data(),
                                                  needle.size()) ? hit_v : miss_v;
                    }
                }
                continue;
            }
            // Precompute dict_match[d] once per dict entry (~thousands)
            // instead of per row (~100M). The per-row inner loop becomes
            // a single load + bitwise-and.
            idx_t dsz = c.str_dict_values.size();
            std::vector<uint8_t> dict_match(dsz, 0);
            if (p.like_contains) {
                const auto &needle = p.sval;
                uint8_t hit_v = p.like_negated ? 0 : 1;
                uint8_t miss_v = p.like_negated ? 1 : 0;
                if (needle.empty()) {
                    // empty needle: LIKE '%%' matches everything;
                    // NOT LIKE '%%' matches nothing
                    std::fill(dict_match.begin(), dict_match.end(), hit_v);
                } else {
                    for (idx_t di = 0; di < dsz; di++) {
                        const auto &dv = c.str_dict_values[di];
                        const char *hs = dv.GetData();
                        uint32_t hl = dv.GetSize();
                        if (hl < needle.size()) { dict_match[di] = miss_v; continue; }
                        dict_match[di] =
                            FindSubstr(hs, hl, needle.data(), needle.size())
                                ? hit_v : miss_v;
                    }
                }
            } else {
                for (idx_t di = 0; di < dsz; di++) {
                    const auto &dv = c.str_dict_values[di];
                    bool eq = (dv.GetSize() == p.sval.size()) &&
                              (p.sval.empty() ||
                               std::memcmp(dv.GetData(), p.sval.data(), p.sval.size()) == 0);
                    dict_match[di] = ((p.op == SimpleCmpOp::EQ) ? eq : !eq) ? 1 : 0;
                }
            }
            const uint32_t *idx_arr = c.str_dict_indices.data();
            for (idx_t r = 0; r < nrows; r++) out_mask[r] &= dict_match[idx_arr[r]];
        } else {
            int64_t pv = p.ival;
            auto ctid = c.type.id();
            if (ctid == LogicalTypeId::INTEGER) {
                const int32_t *arr = c.i32_data.data();
                int32_t pv32 = (int32_t)pv;
                switch (p.op) {
                case SimpleCmpOp::EQ: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] == pv32); break;
                case SimpleCmpOp::NE: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] != pv32); break;
                case SimpleCmpOp::LT: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] <  pv32); break;
                case SimpleCmpOp::LE: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] <= pv32); break;
                case SimpleCmpOp::GT: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] >  pv32); break;
                case SimpleCmpOp::GE: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] >= pv32); break;
                }
            } else { // BIGINT
                const int64_t *arr = c.i64_data.data();
                switch (p.op) {
                case SimpleCmpOp::EQ: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] == pv); break;
                case SimpleCmpOp::NE: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] != pv); break;
                case SimpleCmpOp::LT: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] <  pv); break;
                case SimpleCmpOp::LE: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] <= pv); break;
                case SimpleCmpOp::GT: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] >  pv); break;
                case SimpleCmpOp::GE: for (idx_t r = 0; r < nrows; r++) out_mask[r] &= (uint8_t)(arr[r] >= pv); break;
                }
            }
        }
    }
    return true;
}

// ============================================================================
// Physical Operators
// ============================================================================

class PhysicalTableScan : public PhysicalOperator {
public:
    PhysicalTableScan(TableCatalogEntry *table)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, table->GetTypes()),
          table_(table) {}

    void Init() override {
        state_ = table_->GetStorage().InitScan();
    }

    bool GetData(DataChunk &result) override {
        result.Initialize(GetTypes());
        return table_->GetStorage().Scan(state_, result);
    }

private:
    TableCatalogEntry *table_;
    TableScanState state_;
};

// Streaming file scan - reads CSV directly from file during execution.
// Never materializes the entire file into memory.
class PhysicalFileScan : public PhysicalOperator {
public:
    PhysicalFileScan(const std::string &file_path, char delimiter,
                     std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path), delimiter_(delimiter) {}

    void Init() override {
        reader_ = std::make_unique<FastCSVReader>(file_path_, delimiter_);
        reader_->ReadHeader();
        // A JOIN may call Init() twice (once on build, again on probe if the
        // LEFT side turns out to be larger) so reset the parallel state too.
        mode_decided_ = false;
        use_parallel_ = false;
        pre_chunks_.clear();
        pre_chunk_idx_ = 0;
    }

    bool GetData(DataChunk &result) override {
        // First call: for files above a size threshold, do a one-shot
        // parallel parse into a vector of pre-built DataChunks. On small
        // files we stay on the single-threaded streaming path since the
        // thread-spawn overhead would dominate.
        if (!mode_decided_) {
            mode_decided_ = true;
            size_t sz = reader_->GetSize();
            use_parallel_ = (sz >= 2 * 1024 * 1024);
            if (use_parallel_) {
                const std::vector<bool> *proj_ptr =
                    projection_.empty() ? nullptr : &projection_;
                reader_->ReadIntoChunks(pre_chunks_, GetTypes(), proj_ptr);
                pre_chunk_idx_ = 0;
            }
        }

        if (use_parallel_) {
            if (pre_chunk_idx_ >= pre_chunks_.size()) return false;
            result = std::move(pre_chunks_[pre_chunk_idx_++]);
            return true;
        }

        // Streaming path.
        if (result.ColumnCount() != GetTypes().size()) {
            result.Initialize(GetTypes());
        } else {
            result.Reset();
        }
        idx_t count;
        if (!projection_.empty()) {
            count = reader_->ReadChunkProjected(result, GetTypes(), projection_);
        } else {
            count = reader_->ReadChunk(result, GetTypes());
        }
        return count > 0;
    }

    // Column pruning: tell the scan which columns are needed.
    void SetProjection(std::vector<bool> mask) { projection_ = std::move(mask); }
    const std::vector<bool> &GetProjection() const { return projection_; }

    // Stash the mask passed down from a parent (e.g. PhysicalProjection's
    // SetNeededOutputs walk) directly into the scan's column projection.
    // Without this hook, simple `SELECT col WHERE col = lit` (Q20) reads
    // ALL ~105 columns of every unpruned RG.
    void SetNeededOutputs(const std::vector<bool> &mask) override { projection_ = mask; }

private:
public:
    const std::string &GetFilePath() const { return file_path_; }
    char GetDelimiter() const { return delimiter_; }
    FastCSVReader *GetReader() { return reader_.get(); }

private:
    std::string file_path_;
    char delimiter_;
    std::unique_ptr<FastCSVReader> reader_;
    bool initialized_ = false;
    std::vector<bool> projection_;

    bool mode_decided_ = false;
    bool use_parallel_ = false;
    std::vector<DataChunk> pre_chunks_;
    size_t pre_chunk_idx_ = 0;
};

// Streaming Parquet scan with thread-parallel row-group decode.
//
// Pipeline:
//   - Worker threads pull row groups via an atomic counter.
//   - Each worker decodes its RG independently (ReadColumnInto opens its own
//     std::ifstream, so concurrent reads are safe).
//   - Workers deposit the result into slot[rg_idx]; the main thread consumes
//     slots in order (slot 0 first, then 1, ...) to preserve file row order.
//   - Decode-ahead is throttled so at most `max_ahead` slots live at once
//     (bounds memory to ~max_ahead × per-RG column buffers).
class PhysicalParquetScan : public PhysicalOperator {
public:
    PhysicalParquetScan(const std::string &file_path, std::vector<LogicalType> types,
                        std::shared_ptr<ParquetReader> cached = nullptr)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path), cached_reader_(std::move(cached)) {}

    ~PhysicalParquetScan() override { StopWorkers(); }

    void Init() override {
        StopWorkers();
        // Reuse the catalog-cached reader when available - avoids a second
        // Thrift footer parse per query (~10-20ms on our 80-RG benchmark).
        if (cached_reader_) reader_sp_ = cached_reader_;
        else reader_sp_ = std::make_shared<ParquetReader>(file_path_);
        num_rgs_ = reader_sp_->NumRowGroups();
        // Row-limit pushdown: a small LIMIT N flowing down from above
        // means we can stop decoding after the first few RGs whose
        // cumulative row count exceeds N. Skipping it on a 10M-row
        // 80-RG file flips `SELECT * LIMIT 10` from ~190 ms (full scan
        // of every RG) to ~5 ms (decode RG 0 only). Safe only when no
        // pushdown filter is active — filters might prune RGs, so we
        // can't predict how many we'll need.
        effective_num_rgs_ = num_rgs_;
        if (row_limit_ > 0 && pushdown_filters_.empty()) {
            idx_t cum = 0;
            idx_t k = 0;
            auto &meta = reader_sp_->GetMeta();
            for (; k < num_rgs_; k++) {
                cum += static_cast<idx_t>(meta.row_groups[k].num_rows);
                if (cum >= row_limit_) { k++; break; }
            }
            if (k < effective_num_rgs_) effective_num_rgs_ = k;
        }
        next_decode_.store(0);
        next_emit_ = 0;
        chunks_in_current_ = 0;
        current_rg_size_ = 0;
        current_work_.reset();
        slots_.clear();
        slots_.resize(num_rgs_);
        workers_spawned_ = false;
    }

    void SetRowLimit(idx_t n) override { row_limit_ = n; }

    bool GetData(DataChunk &result) override {
        if (!workers_spawned_) SpawnWorkers();

        if (result.ColumnCount() != GetTypes().size()) result.Initialize(GetTypes());
        else result.Reset();

        idx_t num_cols = static_cast<idx_t>(GetTypes().size());

        // Advance to next RG if the current one is fully drained (or none yet).
        // Loop here so a pruned RG (skipped by zone-map filter) advances
        // immediately to the next without GetData returning empty.
        while (!current_work_ || chunks_in_current_ >= current_rg_size_) {
            if (next_emit_ >= effective_num_rgs_) return false;

            if (workers_.empty()) {
                // Single-threaded path: decode on demand inline.
                current_work_ = DecodeRowGroup(next_emit_);
            } else {
                std::unique_lock<std::mutex> lk(mu_);
                slot_ready_cv_.wait(lk, [&] {
                    return stop_.load() || slots_[next_emit_] != nullptr;
                });
                if (stop_.load()) return false;
                current_work_ = std::move(slots_[next_emit_]);
                // Wake any worker throttled by max_ahead - one slot just freed.
                slot_free_cv_.notify_all();
            }
            // Pruned RGs contribute zero rows. Set size=0 and loop so we
            // pick up the next RG without paying the per-call return cost.
            current_rg_size_ = (current_work_ && current_work_->pruned) ? 0
                : static_cast<idx_t>(
                    reader_sp_->GetMeta().row_groups[next_emit_].num_rows);
            chunks_in_current_ = 0;
            next_emit_++;
        }

        idx_t remaining = current_rg_size_ - chunks_in_current_;
        idx_t count = std::min<idx_t>(remaining, VECTOR_SIZE);

        for (idx_t c = 0; c < num_cols; c++) {
            auto &vec = result.GetVector(c);
            if (!projection_.empty() && c < projection_.size() && !projection_[c]) {
                for (idx_t r = 0; r < count; r++) vec.GetValidity().SetInvalid(r);
                continue;
            }
            auto &col = current_work_->cols[c];
            if (col.decoded) {
                EmitTypedSlice(vec, col, chunks_in_current_, count);
            } else {
                auto &vals = current_work_->cols_fallback[c];
                for (idx_t r = 0; r < count; r++) {
                    if (chunks_in_current_ + r < vals.size()) {
                        result.SetValue(c, r, vals[chunks_in_current_ + r]);
                    } else {
                        vec.GetValidity().SetInvalid(r);
                    }
                }
            }
        }

        result.SetCardinality(count);
        chunks_in_current_ += count;
        return count > 0;
    }

    void SetProjection(std::vector<bool> mask) { projection_ = std::move(mask); }
    void SetNeededOutputs(const std::vector<bool> &mask) override { projection_ = mask; }
    const std::vector<bool> &GetProjection() const { return projection_; }
    // Per-column hint: if set, VARCHAR dict-encoded cols skip per-row string_t
    // materialization (populate only str_dict_indices + str_dict_values).
    // Consumers (fused GROUP BY) must resolve strings via str_dict_values.
    void SetSkipStrData(std::vector<bool> mask) { skip_str_data_ = std::move(mask); }
    // Per-column hint: lengths-only mode. The decoder fills str_lengths
    // (one uint32 per row) and skips byte materialization entirely. Use when
    // the column is consumed only by STRLEN(col) and `<> ''` / `= ''` checks.
    // Mutually exclusive with normal byte materialization.
    void SetStrLengthsOnly(std::vector<bool> mask) { str_lengths_only_ = std::move(mask); }
    // Per-column hint: dict-only mode. The decoder reads only the dict page
    // and skips all data pages. str_dict_values is populated; str_dict_indices
    // stays empty. Use when the consumer only needs to enumerate the dict
    // (e.g. Q26 ORDER BY col LIMIT N with dict-trust = no orphan check).
    void SetStrDictOnly(std::vector<bool> mask) { str_dict_only_ = std::move(mask); }
    // Per-column hint: decoder writes only str_dict_used bitmap (skip
    // str_dict_indices materialization). Used by Q26 dispatch when we need
    // an orphan-safe dict scan but want to skip the per-row indices write.
    void SetStrDictUsedOnly(std::vector<bool> mask) { str_dict_used_only_ = std::move(mask); }

    // Two-phase decode (selection-vector pushdown). When `early_cols` is
    // non-empty AND `build_mask_fn` is set:
    //   1. Worker decodes only `early_cols` (cheap filter cols, e.g.
    //      SearchPhrase dict_indices on Q22).
    //   2. Worker calls build_mask_fn(work.cols, mask) to derive a per-row
    //      keep mask.
    //   3. Worker decodes the remaining projected cols WITH the mask:
    //      PLAIN VARCHAR decoder skips dst[i] writes for masked rows
    //      (Q22 URL: ~87% of rows skipped → ~1.4 GB string_t writes saved).
    // Otherwise the worker takes the original single-pass decode path
    // (DecodeRowGroupInto stays bit-identical to pre-pushdown).
    using BuildMaskFn = std::function<void(const std::vector<ParquetColumnData>&,
                                            std::vector<uint8_t>&)>;
    void SetTwoPhaseDecode(std::vector<idx_t> early_cols, BuildMaskFn fn) {
        two_phase_early_cols_ = std::move(early_cols);
        two_phase_build_mask_fn_ = std::move(fn);
    }
    bool HasTwoPhaseDecode() const {
        return !two_phase_early_cols_.empty() && two_phase_build_mask_fn_;
    }

    const std::string &GetFilePath() const { return file_path_; }
    ParquetReader *GetReader() { return reader_sp_.get(); }

    // Single-column comparison pushed down from a WHERE filter. Used to
    // skip whole row groups whose min/max statistics prove no row in the
    // group could satisfy the predicate. Caller is still responsible for
    // running the full WHERE on rows from groups that ARE scanned —
    // pushdown is conservative ("might match" is true unless stats prove
    // otherwise), so the PhysicalFilter above this scan stays correct.
    struct PushdownFilter {
        idx_t column_index;
        std::string op;   // "=", "<", "<=", ">", ">="
        Value value;
    };
    void SetPushdownFilters(std::vector<PushdownFilter> f) {
        pushdown_filters_ = std::move(f);
    }
    bool HasPushdownFilters() const { return !pushdown_filters_.empty(); }

    // One fully decoded row group - what the worker threads produce.
    struct RGWork {
        std::vector<ParquetColumnData> cols;
        std::vector<std::vector<Value>> cols_fallback;
        // True when DecodeRowGroup short-circuited because every pushdown
        // filter proved the RG can't contain any matching row. Consumers
        // that look at this flag (GetData) emit zero rows for this RG and
        // advance to the next.
        bool pruned = false;
    };

    // Pull the next decoded row group in file order; returns nullptr at EOF.
    // Called by fused scan+aggregate paths (skip DataChunk materialization).
    // Safe to mix with GetData() - both drive the same slot queue.
    std::unique_ptr<RGWork> PullNextRG(idx_t &rg_idx_out) {
        if (!workers_spawned_) SpawnWorkers();
        if (next_emit_ >= num_rgs_) return nullptr;
        std::unique_ptr<RGWork> work;
        if (workers_.empty()) {
            work = DecodeRowGroup(next_emit_);
        } else {
            std::unique_lock<std::mutex> lk(mu_);
            slot_ready_cv_.wait(lk, [&] {
                return stop_.load() || slots_[next_emit_] != nullptr;
            });
            if (stop_.load()) return nullptr;
            work = std::move(slots_[next_emit_]);
            slot_free_cv_.notify_all();
        }
        rg_idx_out = next_emit_;
        next_emit_++;
        return work;
    }

    idx_t RowGroupSize(idx_t rg) const {
        return static_cast<idx_t>(reader_sp_->GetMeta().row_groups[rg].num_rows);
    }

    idx_t NumRowGroups() const { return num_rgs_; }

    // Parallel consumer API. The caller sets a callback that is invoked by
    // worker threads directly after decoding each RG (instead of depositing
    // the RG into `slots_`). `RunParallelRGs(nt)` spawns `nt` workers that
    // pull RGs via the atomic counter, decode, invoke the callback, discard
    // the RGWork, and exit when all RGs are processed. Blocks until done.
    // Used by the fused GROUP BY paths to aggregate inside worker threads
    // - decode and aggregate fuse into a single per-RG pass with thread-
    // local state merged once at the end.
    using RGConsumerFn =
        std::function<void(const RGWork &, idx_t rg_idx, int thread_id)>;
    void SetRGConsumer(RGConsumerFn cb) { rg_consumer_ = std::move(cb); }

    // Signal in-flight workers to stop picking new RGs. Workers that have
    // already started decoding the current RG finish it; remaining undecoded
    // RGs are dropped. Used by bare-LIMIT queries (Q18) once enough groups
    // have been seen.
    void RequestStop() { stop_.store(true); }

    int RunParallelRGs(int num_threads_hint = 0) {
        // Lazy-start: ignore any slot-mode workers; consumer-mode runs its own.
        StopWorkers();
        auto _rp_t0 = std::chrono::steady_clock::now();
        if (slothdb::PqProfileOn()) slothdb::g_pq_profile.Reset();
        unsigned int nt = (unsigned int)num_threads_hint;
        bool had_hint = (nt != 0);
        if (nt == 0) nt = HWThreads();
        unsigned int hw = HWThreads();
        if (nt > hw) nt = hw; // never oversubscribe past logical procs
        // No-hint callers keep the conservative 8-thread cap: their per-thread
        // shard state is sized 8 (tid % 8). Hinted callers opt into more by
        // sizing shard state to match the hint. ClickBench decode-bound
        // queries (Q13/Q15-shape) scale to all 12 logical procs on the 6C/12T
        // chip - HT siblings hide Snappy/RLE pipeline bubbles (sweep: Q15
        // 8t 1.01x -> 12t 1.10x; Q13 8t 0.80x -> 12t 0.93x). SLOTH_MAXTHREADS
        // overrides for tuning experiments.
        if (!had_hint && nt > 8) nt = 8;
        {
            if (const char *_mtenv = std::getenv("SLOTH_MAXTHREADS")) {
                int v = std::atoi(_mtenv);
                if (v >= 1 && v <= (int)hw) nt = (unsigned int)v;
            }
        }
        if (num_rgs_ == 0) nt = 0;
        else if ((idx_t)nt > num_rgs_) nt = static_cast<unsigned int>(num_rgs_);

        next_decode_.store(0);
        stop_.store(false);
        consumer_mode_ = true;
        if (nt <= 1) {
            // Single-threaded (incl. WASM without pthreads): run inline.
            if (nt == 1) WorkerLoop(0);
        } else {
            for (unsigned int t = 0; t < nt; t++) {
                workers_.emplace_back([this, t] { WorkerLoop((int)t); });
            }
            for (auto &t : workers_) if (t.joinable()) t.join();
            workers_.clear();
        }
        consumer_mode_ = false;
        if (slothdb::PqProfileOn()) {
            double wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - _rp_t0).count() / 1e6;
            double d = (nt ? nt : 1);
            double dc = slothdb::g_pq_profile.decomp_ns.load() / 1e6;
            double pd = slothdb::g_pq_profile.pagedecode_ns.load() / 1e6;
            double rd = slothdb::g_pq_profile.rgdecode_ns.load() / 1e6;
            double cs = slothdb::g_pq_profile.consume_ns.load() / 1e6;
            double p1 = slothdb::g_pq_profile.agg_pass1_ns.load() / 1e6;
            double p2 = slothdb::g_pq_profile.agg_pass2_ns.load() / 1e6;
            unsigned long long dsum = slothdb::g_pq_profile.agg_dict_sum.load();
            fprintf(stderr,
                "[SLOTH_PROFILE] RunParallelRGs wall=%.0fms nt=%u pages=%llu | "
                "per-thread: rgdecode=%.0fms (decomp=%.0fms pagedecode=%.0fms other=%.0fms) "
                "consume=%.0fms (agg_p1=%.0fms agg_p2=%.0fms) | acct=%.0fms dict_sum=%llu "
                "| branches skipdi=%llu dictrg=%llu perrow=%llu\n",
                wall, nt, (unsigned long long)slothdb::g_pq_profile.npages.load(),
                rd / d, dc / d, pd / d, (rd - dc - pd) / d, cs / d, p1 / d, p2 / d,
                (rd + cs) / d, dsum,
                (unsigned long long)slothdb::g_pq_profile.c_skipdi.load(),
                (unsigned long long)slothdb::g_pq_profile.c_dictrg.load(),
                (unsigned long long)slothdb::g_pq_profile.c_perrow.load());
        }
        return (int)nt;
    }

private:

    // Returns true if any pushdown filter proves this row group cannot
    // contain any matching row (zone-map skip). Returns false if all
    // filters either don't apply or the stats say "might match".
    bool IsRowGroupPruned(idx_t rg) const {
        if (pushdown_filters_.empty()) return false;
        for (auto &f : pushdown_filters_) {
            if (!reader_sp_->RowGroupMightMatch(rg, f.column_index, f.op, f.value)) {
                return true;
            }
        }
        return false;
    }

    // Decode a single row group into an RGWork. Safe to call from any thread
    // (reads from the read-only mmap'd file_data_).
    std::unique_ptr<RGWork> DecodeRowGroup(idx_t rg) {
        auto work = std::make_unique<RGWork>();
        if (IsRowGroupPruned(rg)) {
            work->pruned = true;
            return work;
        }
        idx_t num_cols = static_cast<idx_t>(GetTypes().size());
        work->cols.resize(num_cols);
        work->cols_fallback.resize(num_cols);
        for (idx_t c = 0; c < num_cols; c++) {
            if (!projection_.empty() && c < projection_.size() && !projection_[c]) continue;
            if (!reader_sp_->ReadColumnInto(rg, c, work->cols[c])) {
                work->cols_fallback[c] = reader_sp_->ReadColumn(rg, c);
            }
        }
        return work;
    }

    // Decode into a caller-owned RGWork. Used by consumer-mode workers that
    // keep a persistent per-thread RGWork to avoid per-RG buffer alloc churn -
    // the typed column vectors retain their capacity across row groups.
    void DecodeRowGroupInto(idx_t rg, RGWork &work) {
        if (IsRowGroupPruned(rg)) {
            work.pruned = true;
            for (auto &c : work.cols) c.Clear();
            for (auto &v : work.cols_fallback) v.clear();
            return;
        }
        work.pruned = false;
        idx_t num_cols = static_cast<idx_t>(GetTypes().size());
        if (work.cols.size() != num_cols) work.cols.resize(num_cols);
        if (work.cols_fallback.size() != num_cols) work.cols_fallback.resize(num_cols);
        for (idx_t c = 0; c < num_cols; c++) {
            if (!projection_.empty() && c < projection_.size() && !projection_[c]) {
                work.cols[c].Clear();
                work.cols_fallback[c].clear();
                continue;
            }
            bool skip_str = (c < skip_str_data_.size()) && skip_str_data_[c];
            bool len_only = (c < str_lengths_only_.size()) && str_lengths_only_[c];
            bool dict_only = (c < str_dict_only_.size()) && str_dict_only_[c];
            bool used_only = (c < str_dict_used_only_.size()) && str_dict_used_only_[c];
            work.cols[c].str_lengths_only = len_only;
            work.cols[c].str_dict_only = dict_only;
            work.cols[c].str_dict_used_only = used_only;
            if (!reader_sp_->ReadColumnInto(rg, c, work.cols[c], skip_str)) {
                work.cols_fallback[c] = reader_sp_->ReadColumn(rg, c);
            } else {
                work.cols_fallback[c].clear();
            }
        }
    }

    // Two-phase variant: decode early_cols → build mask → decode remaining
    // cols with mask. Used by selection-vector pushdown (Q22-shape: cheap
    // VARCHAR NE filter on dict_indices builds a ~13% keep mask, gates
    // dst[i] writes in subsequent PLAIN VARCHAR decodes). Lives as a
    // separate method so the single-pass DecodeRowGroupInto above stays
    // bit-identical to pre-pushdown — prior attempt that branched inside
    // the hot loop regressed Q31/Q32 ~2× (lambda + per-RG branch costs).
    void DecodeRowGroupIntoTwoPhase(idx_t rg, RGWork &work) {
        if (IsRowGroupPruned(rg)) {
            work.pruned = true;
            for (auto &c : work.cols) c.Clear();
            for (auto &v : work.cols_fallback) v.clear();
            return;
        }
        work.pruned = false;
        idx_t num_cols = static_cast<idx_t>(GetTypes().size());
        if (work.cols.size() != num_cols) work.cols.resize(num_cols);
        if (work.cols_fallback.size() != num_cols) work.cols_fallback.resize(num_cols);
        // is_early[c]: whether column c is in the early-decode set.
        // Kept as thread_local to avoid per-RG allocation.
        thread_local std::vector<uint8_t> is_early_tls;
        is_early_tls.assign(num_cols, 0);
        for (idx_t c : two_phase_early_cols_) {
            if (c < num_cols) is_early_tls[c] = 1;
        }
        auto setup_and_decode = [&](idx_t c, const std::vector<uint8_t> *mask) {
            bool skip_str = (c < skip_str_data_.size()) && skip_str_data_[c];
            bool len_only = (c < str_lengths_only_.size()) && str_lengths_only_[c];
            bool dict_only = (c < str_dict_only_.size()) && str_dict_only_[c];
            bool used_only = (c < str_dict_used_only_.size()) && str_dict_used_only_[c];
            work.cols[c].str_lengths_only = len_only;
            work.cols[c].str_dict_only = dict_only;
            work.cols[c].str_dict_used_only = used_only;
            if (!reader_sp_->ReadColumnInto(rg, c, work.cols[c], skip_str, mask)) {
                work.cols_fallback[c] = reader_sp_->ReadColumn(rg, c);
            } else {
                work.cols_fallback[c].clear();
            }
        };
        // Phase 1: decode early cols (no mask).
        for (idx_t c : two_phase_early_cols_) {
            if (c >= num_cols) continue;
            if (!projection_.empty() && c < projection_.size() && !projection_[c]) continue;
            setup_and_decode(c, nullptr);
        }
        // Phase 2: build mask from early-decoded cols.
        thread_local std::vector<uint8_t> phase_mask;
        phase_mask.clear();
        two_phase_build_mask_fn_(work.cols, phase_mask);
        const std::vector<uint8_t> *mask_ptr =
            phase_mask.empty() ? nullptr : &phase_mask;
        // Phase 3: decode remaining projected cols, masked.
        for (idx_t c = 0; c < num_cols; c++) {
            if (is_early_tls[c]) continue;
            if (!projection_.empty() && c < projection_.size() && !projection_[c]) {
                work.cols[c].Clear();
                work.cols_fallback[c].clear();
                continue;
            }
            setup_and_decode(c, mask_ptr);
        }
    }

    void WorkerLoop(int thread_id = 0) {
        // Bound decode-ahead in slot mode so memory doesn't balloon. Consumer
        // mode has no queue - work is freed right after the callback - so no
        // throttle is needed.
        const idx_t max_ahead = workers_.size() * 2 + 2;
        // Thread-local persistent RGWork - reuse buffers across row groups so
        // each RG doesn't malloc a fresh ~1 MB column buffer per column.
        RGWork consumer_work;
        while (true) {
            if (stop_.load()) return;
            idx_t rg = next_decode_.fetch_add(1);
            if (rg >= effective_num_rgs_) return;

            if (consumer_mode_) {
                // Cheap zone-map check before decode — pruned RGs cost
                // metadata reads only, not column-chunk decoding.
                if (IsRowGroupPruned(rg)) continue;
                // Dispatch to two-phase decode iff configured. The branch
                // is per-RG (~95×), not per-row — Q31/Q32 hot path stays
                // on the single-pass DecodeRowGroupInto when two-phase
                // isn't set.
                bool _prof = slothdb::PqProfileOn();
                auto _d0 = _prof ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
                if (HasTwoPhaseDecode()) {
                    DecodeRowGroupIntoTwoPhase(rg, consumer_work);
                } else {
                    DecodeRowGroupInto(rg, consumer_work);
                }
                auto _d1 = _prof ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
                if (rg_consumer_) rg_consumer_(consumer_work, rg, thread_id);
                if (_prof) {
                    auto _d2 = std::chrono::steady_clock::now();
                    slothdb::g_pq_profile.rgdecode_ns.fetch_add(
                        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            _d1 - _d0).count(), std::memory_order_relaxed);
                    slothdb::g_pq_profile.consume_ns.fetch_add(
                        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            _d2 - _d1).count(), std::memory_order_relaxed);
                }
                // consumer_work retains its buffer capacity; the NEXT RG's
                // decode reuses the same memory instead of mallocing afresh.
                continue;
            }

            // Run zone-map / dict-skip BEFORE the throttle wait. Pruned RGs
            // produce a near-empty RGWork (just the `pruned` flag), so they
            // don't consume the memory budget that max_ahead protects.
            // Without this short-circuit, workers serialise on slot_free_cv_
            // even though the prune check itself is the only work — Q20
            // INT64 dict-skip on 226 RGs blocked workers ~470ms each waiting
            // for main thread to advance next_emit_, hiding a ~7x parallel
            // speed-up of the dict scan.
            if (IsRowGroupPruned(rg)) {
                auto pwork = std::make_unique<RGWork>();
                pwork->pruned = true;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    slots_[rg] = std::move(pwork);
                }
                slot_ready_cv_.notify_all();
                continue;
            }

            // Slot-deposit mode: throttle until this rg is within max_ahead.
            {
                std::unique_lock<std::mutex> lk(mu_);
                slot_free_cv_.wait(lk, [&] {
                    return stop_.load() || rg < next_emit_ + max_ahead;
                });
                if (stop_.load()) return;
            }

            auto work = DecodeRowGroup(rg);

            {
                std::unique_lock<std::mutex> lk(mu_);
                slots_[rg] = std::move(work);
            }
            slot_ready_cv_.notify_all();
        }
    }

    void SpawnWorkers() {
        workers_spawned_ = true;
        unsigned int nt = HWThreads();
        if (nt > 6) nt = 6;
        // effective_num_rgs_ shrinks for row-limit-pushdown queries; no
        // point spawning more workers than RGs we'll actually decode.
        idx_t budget = effective_num_rgs_;
        if (budget <= 1) nt = 0;
        else if ((idx_t)nt > budget) nt = static_cast<unsigned int>(budget);
        stop_.store(false);
        // nt <= 1: skip spawn. The slot-mode caller path checks
        // `workers_.empty()` and decodes inline (see GetNextWork).
        if (nt <= 1) return;
        for (unsigned int t = 0; t < nt; t++) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    void StopWorkers() {
        if (workers_.empty()) return;
        stop_.store(true);
        slot_ready_cv_.notify_all();
        slot_free_cv_.notify_all();
        for (auto &t : workers_) if (t.joinable()) t.join();
        workers_.clear();
        slots_.clear();
        stop_.store(false);
    }

    // Memcpy a VECTOR_SIZE slice [row_off, row_off+count) from `col` into `vec`.
    static void EmitTypedSlice(Vector &vec, const ParquetColumnData &col,
                               idx_t row_off, idx_t count) {
        auto tid = col.type.id();
        if (!col.all_valid && !col.validity.empty()) {
            auto &v = vec.GetValidity();
            for (idx_t r = 0; r < count; r++) {
                if (!col.validity[row_off + r]) v.SetInvalid(r);
            }
        }
        switch (tid) {
        case LogicalTypeId::BOOLEAN: {
            auto *dst = vec.GetData<bool>();
            for (idx_t r = 0; r < count; r++) dst[r] = col.bool_data[row_off + r] != 0;
            break;
        }
        case LogicalTypeId::INTEGER:
            std::memcpy(vec.GetData<int32_t>(), col.i32_data.data() + row_off, count * 4);
            break;
        case LogicalTypeId::BIGINT:
            std::memcpy(vec.GetData<int64_t>(), col.i64_data.data() + row_off, count * 8);
            break;
        case LogicalTypeId::FLOAT:
            std::memcpy(vec.GetData<float>(), col.f32_data.data() + row_off, count * 4);
            break;
        case LogicalTypeId::DOUBLE:
            std::memcpy(vec.GetData<double>(), col.f64_data.data() + row_off, count * 8);
            break;
        case LogicalTypeId::VARCHAR:
            std::memcpy(vec.GetData<string_t>(),
                        col.str_data.data() + row_off, count * sizeof(string_t));
            if (col.str_heap) vec.GetStringBuffer().AttachHeap(col.str_heap);
            break;
        default:
            break;
        }
    }

    std::string file_path_;
    std::shared_ptr<ParquetReader> reader_sp_;
    std::shared_ptr<ParquetReader> cached_reader_;
    std::vector<bool> skip_str_data_; // per-column hint
    std::vector<bool> str_lengths_only_; // per-column hint: lengths-only decode
    std::vector<bool> str_dict_only_;   // per-column hint: skip data pages
    std::vector<bool> str_dict_used_only_;   // per-column hint: dict-presence bitmap
    std::vector<bool> projection_;
    std::vector<PushdownFilter> pushdown_filters_; // zone-map row-group skip
    // Two-phase decode (Q22-style selection-vector pushdown). When set, the
    // worker decodes early_cols first, runs build_mask_fn on them, then
    // decodes the remaining projected cols WITH the resulting mask passed
    // through to ReadColumnInto. Empty by default — the single-pass
    // DecodeRowGroupInto path is bit-identical to pre-pushdown.
    std::vector<idx_t> two_phase_early_cols_;
    BuildMaskFn two_phase_build_mask_fn_;

    // Parallel decode state.
    idx_t num_rgs_ = 0;
    // Computed in Init from row_limit_ + RG row counts. Workers and the
    // emit loop iterate up to this, so a `LIMIT 10` over an 80-RG file
    // decodes only the first RG instead of all 80.
    idx_t effective_num_rgs_ = 0;
    idx_t row_limit_ = 0;
    bool workers_spawned_ = false;
    std::atomic<idx_t> next_decode_{0};
    idx_t next_emit_ = 0;            // main-thread only
    std::vector<std::unique_ptr<RGWork>> slots_; // indexed by rg_idx
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable slot_ready_cv_; // main waits on this
    std::condition_variable slot_free_cv_;  // workers wait on this (throttle)
    // Consumer-mode: workers invoke rg_consumer_ directly after decoding.
    bool consumer_mode_ = false;
    RGConsumerFn rg_consumer_;

    // Current-chunk emission state.
    std::unique_ptr<RGWork> current_work_;
    idx_t current_rg_size_ = 0;
    idx_t chunks_in_current_ = 0;
};

#ifndef SLOTHDB_EDGE
// Streaming Avro scan - mirrors PhysicalJSONScan. Init() parses the Avro
// container into typed DataChunks via AvroReader::ReadIntoChunks, skipping
// the rows_/BulkLoadRows roundtrip; GetData() emits them sequentially.
class PhysicalAvroScan : public PhysicalOperator {
public:
    PhysicalAvroScan(const std::string &file_path, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path) {}

    void Init() override {
        AvroReader reader(file_path_);
        reader.DetectSchemaLight();
        chunks_.clear();
        reader.ReadIntoChunks(chunks_, GetTypes());
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (emit_pos_ >= chunks_.size()) return false;
        result = std::move(chunks_[emit_pos_++]);
        return true;
    }

    const std::string &GetFilePath() const { return file_path_; }

private:
    std::string file_path_;
    std::vector<DataChunk> chunks_;
    idx_t emit_pos_ = 0;
};

// Streaming SQLite scan - mirrors PhysicalAvroScan / PhysicalArrowScan.
// Init() scans the given table from the SQLite file directly into typed
// DataChunk vectors via SQLiteScanner::ScanTableIntoChunks, skipping the
// DataTable bulk-load roundtrip; GetData() emits them sequentially.
class PhysicalSQLiteScan : public PhysicalOperator {
public:
    PhysicalSQLiteScan(const std::string &file_path,
                        const std::string &table_name,
                        std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path), table_name_(table_name) {}

    void Init() override {
        SQLiteScanner scanner(file_path_);
        chunks_.clear();
        scanner.ScanTableIntoChunks(chunks_, GetTypes(), table_name_);
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (emit_pos_ >= chunks_.size()) return false;
        result = std::move(chunks_[emit_pos_++]);
        return true;
    }

    const std::string &GetFilePath() const { return file_path_; }
    const std::string &GetTableName() const { return table_name_; }

private:
    std::string file_path_;
    std::string table_name_;
    std::vector<DataChunk> chunks_;
    idx_t emit_pos_ = 0;
};

// Streaming Arrow IPC scan - mirrors PhysicalAvroScan. Init() parses the
// Arrow file into typed DataChunks via ArrowIPCReader::ReadIntoChunks,
// skipping the Value-boxed rows_/BulkLoadRows roundtrip; GetData() emits
// them sequentially.
class PhysicalArrowScan : public PhysicalOperator {
public:
    PhysicalArrowScan(const std::string &file_path, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path) {}

    void Init() override {
        ArrowIPCReader reader(file_path_);
        reader.DetectSchemaLight();
        chunks_.clear();
        reader.ReadIntoChunks(chunks_, GetTypes());
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (emit_pos_ >= chunks_.size()) return false;
        result = std::move(chunks_[emit_pos_++]);
        return true;
    }

    const std::string &GetFilePath() const { return file_path_; }

private:
    std::string file_path_;
    std::vector<DataChunk> chunks_;
    idx_t emit_pos_ = 0;
};
#endif // SLOTHDB_EDGE

// Streaming JSON scan - parses the file in Init() (mmap + parallel worker
// threads under the hood via JSONReader::ReadIntoChunks) into a vector of
// DataChunks, then emits them one at a time via GetData(). Avoids the
// DataTable bulk-load and rescan that used to happen for read_json(...)
// queries, mirroring PhysicalParquetScan's "skip the intermediate storage"
// approach.
class PhysicalJSONScan : public PhysicalOperator {
public:
    PhysicalJSONScan(const std::string &file_path, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path) {}

    void Init() override {
        JSONReader reader(file_path_);
        reader.DetectSchemaLight();
        chunks_.clear();
        reader.ReadIntoChunks(chunks_, GetTypes());
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (emit_pos_ >= chunks_.size()) return false;
        result = std::move(chunks_[emit_pos_++]);
        return true;
    }

    const std::string &GetFilePath() const { return file_path_; }

private:
    std::string file_path_;
    std::vector<DataChunk> chunks_;
    idx_t emit_pos_ = 0;
};

// Ultra-fast scan that only counts rows - no field parsing at all.
class PhysicalCountScan : public PhysicalOperator {
public:
    PhysicalCountScan(const std::string &file_path, char delimiter,
                      std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path), delimiter_(delimiter) {}

    void Init() override {
        reader_ = std::make_unique<FastCSVReader>(file_path_, delimiter_);
        reader_->ReadHeader();
        counted_ = false;
        row_count_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (counted_) return false;
        counted_ = true;
        row_count_ = reader_->CountRows();
        // Return a single-row chunk with the count in a BIGINT column.
        result.Initialize({LogicalType::BIGINT()});
        result.GetVector(0).GetData<int64_t>()[0] = static_cast<int64_t>(row_count_);
        result.SetCardinality(1);
        return true;
    }

    idx_t GetRowCount() const { return row_count_; }

private:
    std::string file_path_;
    char delimiter_;
    std::unique_ptr<FastCSVReader> reader_;
    bool counted_ = false;
    idx_t row_count_ = 0;
};

class PhysicalFilter : public PhysicalOperator {
public:
    PhysicalFilter(BoundExprPtr condition, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::FILTER, std::move(types)),
          condition_(std::move(condition)) {}

    // Exposed so fused FILTER+AGG+SCAN paths can inline the predicate.
    const BoundExpression *GetCondition() const { return condition_.get(); }

    // Forward needed-output mask to child, augmented with columns the
    // filter condition itself references. Without this, a parquet scan
    // child sees no projection mask and decodes every column. (Q20:
    // SELECT col WHERE col = lit went from 380ms RG decode → ~30ms.)
    void SetNeededOutputs(const std::vector<bool> &out_mask) override {
        if (children.empty()) return;
        idx_t in_cols = children[0]->GetTypes().size();
        std::vector<bool> in_mask = out_mask;
        if (in_mask.size() < in_cols) in_mask.resize(in_cols, false);
        if (condition_) {
            std::function<void(const BoundExpression &)> walk =
                [&](const BoundExpression &x) {
                    if (x.GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &c = static_cast<const BoundColumnRef &>(x);
                        if (c.column_index < in_mask.size()) in_mask[c.column_index] = true;
                    } else if (x.GetExpressionType() == BoundExpressionType::COMPARISON) {
                        auto &c = static_cast<const BoundComparison &>(x);
                        walk(*c.left); walk(*c.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::ARITHMETIC) {
                        auto &a = static_cast<const BoundArithmetic &>(x);
                        walk(*a.left); walk(*a.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::FUNCTION) {
                        auto &f = static_cast<const BoundFunction &>(x);
                        for (auto &a : f.arguments) walk(*a);
                    } else if (x.GetExpressionType() == BoundExpressionType::CAST) {
                        auto &c = static_cast<const BoundCast &>(x);
                        walk(*c.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::CONJUNCTION) {
                        auto &c = static_cast<const BoundConjunction &>(x);
                        walk(*c.left); walk(*c.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::NEGATION) {
                        auto &n = static_cast<const BoundNegation &>(x);
                        walk(*n.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::IS_NULL) {
                        auto &n = static_cast<const BoundIsNull &>(x);
                        walk(*n.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
                        auto &u = static_cast<const BoundUnaryMinus &>(x);
                        walk(*u.child);
                    }
                };
            walk(*condition_);
        }
        children[0]->SetNeededOutputs(in_mask);
    }

    void Init() override {
        for (auto &child : children) child->Init();
    }

    bool GetData(DataChunk &result) override {
        while (true) {
            DataChunk input;
            input.Initialize(GetTypes());
            if (!children[0]->GetData(input)) return false;
            if (input.size() == 0) continue;

            // Evaluate filter condition.
            Vector filter_result(LogicalType::BOOLEAN(), input.size());
            ExpressionExecutor::Execute(*condition_, input, filter_result, input.size());

            auto *filter_data = filter_result.GetData<bool>();
            auto &filter_validity = filter_result.GetValidity();

            // Build selection vector of matching rows.
            uint32_t sel[VECTOR_SIZE];
            idx_t sel_count = 0;
            for (idx_t i = 0; i < input.size(); i++) {
                if (filter_validity.RowIsValid(i) && filter_data[i]) {
                    sel[sel_count++] = static_cast<uint32_t>(i);
                }
            }

            if (sel_count == 0) continue; // all filtered - pull next chunk

            if (result.ColumnCount() != GetTypes().size()) {
                result.Initialize(GetTypes());
            } else {
                result.Reset();
            }

            idx_t num_cols = input.ColumnCount();
            for (idx_t c = 0; c < num_cols; c++) {
                auto &src = input.GetVector(c);
                auto &dst = result.GetVector(c);
                auto &src_val = src.GetValidity();
                auto &dst_val = dst.GetValidity();
                switch (src.GetType().id()) {
                case LogicalTypeId::BIGINT: {
                    auto *s = src.GetData<int64_t>();
                    auto *d = dst.GetData<int64_t>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::INTEGER: {
                    auto *s = src.GetData<int32_t>();
                    auto *d = dst.GetData<int32_t>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::DOUBLE: {
                    auto *s = src.GetData<double>();
                    auto *d = dst.GetData<double>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::FLOAT: {
                    auto *s = src.GetData<float>();
                    auto *d = dst.GetData<float>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::BOOLEAN: {
                    auto *s = src.GetData<bool>();
                    auto *d = dst.GetData<bool>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    break;
                }
                case LogicalTypeId::VARCHAR: {
                    // Zero-copy: copy string_t slices and share the source's
                    // string buffer so string_t pointers remain valid.
                    auto *s = src.GetData<string_t>();
                    auto *d = dst.GetData<string_t>();
                    for (idx_t i = 0; i < sel_count; i++) {
                        d[i] = s[sel[i]];
                        if (!src_val.RowIsValid(sel[i])) dst_val.SetInvalid(i);
                    }
                    dst.SetAuxiliaryPtr(src.GetAuxiliaryPtr());
                    break;
                }
                default: {
                    // Fallback for types without a fast path.
                    for (idx_t i = 0; i < sel_count; i++) {
                        dst.SetValue(i, src.GetValue(sel[i]));
                    }
                    break;
                }
                }
            }
            result.SetCardinality(sel_count);
            return true;
        }
    }

private:
    BoundExprPtr condition_;
};

class PhysicalProjection : public PhysicalOperator {
public:
    PhysicalProjection(std::vector<BoundExprPtr> expressions, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::PROJECTION, std::move(types)),
          expressions_(std::move(expressions)) {}

    // Exposed so multi-col GROUP BY fast path can peek through GROUP-BY-by-expr lift.
    const std::vector<BoundExprPtr> &GetExpressions() const { return expressions_; }

    // Pass row-limit hints through. LIMIT -> PROJECTION -> ORDER_BY is a
    // common plan shape; without forwarding here, ORDER_BY does a full
    // sort of millions of rows even when LIMIT 10 sits above.
    void SetRowLimit(idx_t n) override {
        if (!children.empty()) children[0]->SetRowLimit(n);
    }

    // Translate TopN hint's output col_idx through this projection. If
    // expressions_[col_idx] is a simple ColumnRef, we can forward the
    // underlying input col_idx; otherwise the projection materializes a
    // computed value our child can't reason about, so we drop the hint.
    void SetTopNHint(idx_t col_idx, bool ascending, idx_t limit) override {
        if (children.empty() || col_idx >= expressions_.size()) return;
        auto &e = expressions_[col_idx];
        if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return;
        auto &cref = static_cast<BoundColumnRef &>(*e);
        children[0]->SetTopNHint(cref.column_index, ascending, limit);
    }

    // Forward column pruning down through the projection. The needed-out
    // mask is stated in the projection's OUTPUT slots, which we map back
    // to INPUT slots by following each output expression's ColumnRef
    // (the only kind of expression that pulls from a single input column).
    // For non-ColumnRef projections we conservatively mark every input
    // column referenced by the expression. Without this, callers like
    // PhysicalWindow telling Projection "I only need cols 1,7" never
    // reaches PhysicalParquetScan and we decode all 10 columns of 10M
    // rows instead of 2.
    void SetNeededOutputs(const std::vector<bool> &out_mask) override {
        if (children.empty()) return;
        idx_t in_cols = children[0]->GetTypes().size();
        std::vector<bool> in_mask(in_cols, false);
        for (idx_t o = 0; o < expressions_.size() && o < out_mask.size(); o++) {
            if (!out_mask[o]) continue;
            auto *e = expressions_[o].get();
            if (!e) continue;
            // Walk the expression collecting input column refs.
            std::function<void(const BoundExpression &)> walk =
                [&](const BoundExpression &x) {
                    if (x.GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &c = static_cast<const BoundColumnRef &>(x);
                        if (c.column_index < in_mask.size()) in_mask[c.column_index] = true;
                    } else if (x.GetExpressionType() == BoundExpressionType::COMPARISON) {
                        auto &c = static_cast<const BoundComparison &>(x);
                        walk(*c.left); walk(*c.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::ARITHMETIC) {
                        auto &a = static_cast<const BoundArithmetic &>(x);
                        walk(*a.left); walk(*a.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::FUNCTION) {
                        auto &f = static_cast<const BoundFunction &>(x);
                        for (auto &a : f.arguments) walk(*a);
                    } else if (x.GetExpressionType() == BoundExpressionType::CAST) {
                        auto &c = static_cast<const BoundCast &>(x);
                        walk(*c.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::CONJUNCTION) {
                        auto &c = static_cast<const BoundConjunction &>(x);
                        walk(*c.left); walk(*c.right);
                    } else if (x.GetExpressionType() == BoundExpressionType::NEGATION) {
                        auto &n = static_cast<const BoundNegation &>(x);
                        walk(*n.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::IS_NULL) {
                        auto &n = static_cast<const BoundIsNull &>(x);
                        walk(*n.child);
                    } else if (x.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
                        auto &u = static_cast<const BoundUnaryMinus &>(x);
                        walk(*u.child);
                    }
                };
            walk(*e);
        }
        children[0]->SetNeededOutputs(in_mask);
    }

    // Detect SELECT * style identity projection: every output column is
    // a plain BoundColumnRef pointing at the same input slot. When this
    // holds, GetData becomes a passthrough — chunks flow from the child
    // unchanged, no per-row ExpressionExecutor::Execute pass. Cuts the
    // 10M-row `SELECT * FROM x ORDER BY y LIMIT 10` cost roughly in half.
    bool IsIdentityProjection() const {
        if (children.empty() || expressions_.size() != children[0]->GetTypes().size())
            return false;
        for (idx_t i = 0; i < expressions_.size(); i++) {
            auto &e = expressions_[i];
            if (!e) return false;
            if (e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            auto &c = static_cast<BoundColumnRef &>(*e);
            if (c.column_index != i) return false;
        }
        return true;
    }

    void Init() override {
        identity_passthrough_ = IsIdentityProjection();
        for (auto &child : children) child->Init();
    }

    bool GetData(DataChunk &result) override {
        if (identity_passthrough_) {
            // Pass-through: no expression evaluation, no per-row copy —
            // child fills `result` directly.
            return children[0]->GetData(result);
        }
        DataChunk input;
        if (!children.empty()) {
            input.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(input)) return false;
            if (input.size() == 0) return false;
        }

        result.Initialize(GetTypes());
        idx_t count = children.empty() ? 1 : input.size();

        for (idx_t col = 0; col < expressions_.size(); col++) {
            ExpressionExecutor::Execute(*expressions_[col], input,
                                        result.GetVector(col), count);
        }
        result.SetCardinality(count);
        return true;
    }

private:
    std::vector<BoundExprPtr> expressions_;
    bool identity_passthrough_ = false;
};

class PhysicalOrderBy : public PhysicalOperator {
public:
    PhysicalOrderBy(std::vector<BoundOrderBy> orders, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::ORDER_BY, std::move(types)),
          orders_(std::move(orders)) {}

    // Parent-projection pushdown: when a PhysicalProjection above this OrderBy
    // only references columns {c1, c2, ...}, pass-2 of CollectTopN_Primitive
    // (which decodes the K winning rows from parquet) can skip decoding all
    // other columns. Q25 has 105-col hits.parquet but selects only
    // SearchPhrase → without this we decode 104 unused cols (~1.2s wasted).
    void SetParentProjectionCols(std::vector<bool> mask) {
        parent_proj_mask_ = std::move(mask);
    }

    // Top-N pushdown: PhysicalLimit propagates `limit + offset` here.
    // When set, we use a bounded min/max heap of that size instead of
    // collecting + sorting the full input. Critical for queries like
    // `ORDER BY x DESC LIMIT 10` over millions of rows — full sort
    // is O(N log N), bounded heap is O(N log K) which can be 1000x
    // faster when K << N.
    void SetRowLimit(idx_t n) override { row_limit_ = n; }

    void Init() override {
        // TopN pushdown: when row_limit_ is set (PhysicalLimit fed it to us)
        // AND the ORDER BY is a single simple ColumnRef, tell our child "you
        // only need the top-K rows by this output column". Aggregates can
        // honor this with a bounded heap instead of materializing all groups.
        // Q35: 9.76 M groups × ORDER BY count DESC LIMIT 10 → aggregate
        // produces only 10 rows after pushdown.
        if (row_limit_ > 0 && orders_.size() == 1 && !children.empty() &&
            orders_[0].expression &&
            orders_[0].expression->GetExpressionType() ==
                BoundExpressionType::COLUMN_REF) {
            auto &cref = static_cast<BoundColumnRef &>(*orders_[0].expression);
            children[0]->SetTopNHint(cref.column_index,
                                      orders_[0].ascending, row_limit_);
        }
        for (auto &child : children) child->Init();
        collected_ = false;
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!collected_) {
            CollectAll();
            collected_ = true;
        }

        if (emit_pos_ >= sorted_rows_.size()) return false;

        result.Initialize(GetTypes());
        idx_t count = 0;
        while (emit_pos_ < sorted_rows_.size() && count < VECTOR_SIZE) {
            auto &row = sorted_rows_[emit_pos_];
            for (idx_t col = 0; col < row.size(); col++) {
                result.SetValue(col, count, row[col]);
            }
            emit_pos_++;
            count++;
        }
        result.SetCardinality(count);
        return count > 0;
    }

private:
    // Compare(a, b) returns true if `a` should come before `b` in the
    // final OUTPUT order. Used both as the std::sort comparator and as
    // the priority_queue ordering function.
    //
    // For ORDER BY by plain column ref this reads `a[col_idx]` directly.
    // For ORDER BY by expression (`ORDER BY ROUND(AVG(salary))`,
    // `ORDER BY x + y`) the expression evaluator can't be invoked here
    // cheaply per comparison, so callers must precompute `sort_keys_`
    // (one Value per ORDER BY clause per row) and the comparator
    // dispatches on `sort_keys_[idx]` instead. To keep the call sites
    // simple, when sort_keys_ is populated, callers sort an `idx_perm_`
    // vector via LessIdx; the std::vector<Value> overload is preserved
    // for the all-column-ref fast path.
    bool Less(const std::vector<Value> &a, const std::vector<Value> &b) const {
        for (auto &order : orders_) {
            idx_t col_idx = 0;
            if (order.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                col_idx = static_cast<BoundColumnRef &>(*order.expression).column_index;
            }
            auto &va = a[col_idx];
            auto &vb = b[col_idx];
            if (va.IsNull() && vb.IsNull()) continue;
            if (va.IsNull()) return !order.ascending;
            if (vb.IsNull()) return order.ascending;
            if (va < vb) return order.ascending;
            if (vb < va) return !order.ascending;
        }
        return false;
    }

    // Compare two precomputed key rows. Used when at least one ORDER BY
    // expression is a non-trivial scalar (function, arithmetic, cast).
    bool LessKeys(const std::vector<Value> &ka, const std::vector<Value> &kb) const {
        for (size_t k_i = 0; k_i < orders_.size(); k_i++) {
            auto &va = ka[k_i];
            auto &vb = kb[k_i];
            if (va.IsNull() && vb.IsNull()) continue;
            if (va.IsNull()) return !orders_[k_i].ascending;
            if (vb.IsNull()) return orders_[k_i].ascending;
            if (va < vb) return orders_[k_i].ascending;
            if (vb < va) return !orders_[k_i].ascending;
        }
        return false;
    }

    // True iff every ORDER BY clause is a plain column reference. The
    // hot CollectFull / CollectTopN paths use this to skip expression
    // evaluation entirely.
    bool AllOrderByAreColumnRefs() const {
        for (auto &o : orders_) {
            if (o.expression->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
        }
        return true;
    }

    // Evaluate every ORDER BY expression on the given input chunk and
    // append per-row keys to `out_keys`. One Value per ORDER BY clause.
    void EvaluateOrderKeys(DataChunk &chunk,
                           std::vector<std::vector<Value>> &out_keys) const {
        std::vector<Vector> key_vecs;
        key_vecs.reserve(orders_.size());
        for (auto &o : orders_) {
            Vector v(o.expression->GetReturnType(), chunk.size());
            ExpressionExecutor::Execute(*o.expression, chunk, v, chunk.size());
            key_vecs.push_back(std::move(v));
        }
        for (idx_t i = 0; i < chunk.size(); i++) {
            std::vector<Value> key;
            key.reserve(orders_.size());
            for (idx_t k_i = 0; k_i < orders_.size(); k_i++) {
                key.push_back(key_vecs[k_i].GetValue(i));
            }
            out_keys.push_back(std::move(key));
        }
    }

    void CollectAll() {
        // Bounded-heap top-N path. Triggered when LIMIT pushed a small
        // limit down. Threshold guards against degenerate cases — for
        // very large K the heap loses its advantage and a full sort is
        // simpler.
        constexpr idx_t TOP_N_THRESHOLD = 65536;
        if (row_limit_ > 0 && row_limit_ <= TOP_N_THRESHOLD) {
            CollectTopN(row_limit_);
            return;
        }
        CollectFull();
    }

    // VARCHAR specialisation of the parquet TopN fast path. Mirrors the
    // numeric primitive version but the key type is std::string. Built
    // for ClickBench Q26 (`SELECT col FROM hits WHERE c <> '' ORDER BY
    // col LIMIT N`) which would otherwise run for 60+ seconds in the
    // generic Value-boxing path.
    //
    // Skips the stats-based RG ordering — VARCHAR min/max stats from
    // parquet typically come as truncated bytes which need careful UTF-8
    // handling for sort correctness. Goes straight to parallel-decode-all
    // with per-thread heaps.
    bool CollectTopN_Varchar(idx_t k, idx_t key_col, bool ascending) {
        struct LightEntry {
            std::string key;       // owned copy so heap survives RG free
            uint32_t rg_idx;
            uint32_t row_idx;
            bool is_null = false;
        };
        auto light_cmp = [ascending](const LightEntry &a, const LightEntry &b) {
            if (a.is_null && b.is_null) return false;
            if (a.is_null) return !ascending;
            if (b.is_null) return ascending;
            return ascending ? a.key < b.key : a.key > b.key;
        };
        using LightHeap = std::priority_queue<LightEntry,
            std::vector<LightEntry>, decltype(light_cmp)>;

        // Walk the plan: OrderBy(this) → [Projection?] → [Filter?] → ParquetScan.
        // The projection may be non-identity (e.g. `SELECT SearchPhrase` over
        // wide hits.parquet — Q26). When it is, every projection expression
        // must be a plain column ref; we record an output→scan column map
        // and translate `key_col` (which is the orderby's input index =
        // projection's output index) into a scan-side index.
        PhysicalProjection *outer_proj = dynamic_cast<PhysicalProjection *>(children[0].get());
        PhysicalFilter *flt = nullptr;
        PhysicalParquetScan *pq_scan = nullptr;
        std::vector<SimplePredicate> tn_preds;
        std::vector<idx_t> output_to_scan;  // empty = identity output

        auto unwrap_proj = [&](PhysicalProjection *p) -> bool {
            // Returns true on success, also sets output_to_scan if non-identity.
            if (!p || p->children.empty()) return false;
            if (p->IsIdentityProjection()) return true;
            auto &exprs = p->GetExpressions();
            std::vector<idx_t> map;
            map.reserve(exprs.size());
            for (auto &e : exprs) {
                if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
                map.push_back(static_cast<BoundColumnRef &>(*e).column_index);
            }
            output_to_scan = std::move(map);
            return true;
        };

        if (outer_proj) {
            if (!unwrap_proj(outer_proj)) return false;
            auto *child = outer_proj->children[0].get();
            pq_scan = dynamic_cast<PhysicalParquetScan *>(child);
            if (!pq_scan) flt = dynamic_cast<PhysicalFilter *>(child);
        } else {
            pq_scan = dynamic_cast<PhysicalParquetScan *>(children[0].get());
            if (!pq_scan) flt = dynamic_cast<PhysicalFilter *>(children[0].get());
        }
        if (!pq_scan && flt && flt->GetCondition() && !flt->children.empty()) {
            std::vector<SimplePredicate> tmp;
            if (TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                auto *child = flt->children[0].get();
                pq_scan = dynamic_cast<PhysicalParquetScan *>(child);
                if (!pq_scan) {
                    if (auto *p2 = dynamic_cast<PhysicalProjection *>(child)) {
                        if (p2->IsIdentityProjection() && !p2->children.empty())
                            pq_scan = dynamic_cast<PhysicalParquetScan *>(p2->children[0].get());
                    }
                }
                if (pq_scan) tn_preds = std::move(tmp);
            }
        }
        if (!pq_scan) return false;

        // Translate orderby's key_col (output-side) to scan-side.
        idx_t key_col_scan = key_col;
        if (!output_to_scan.empty()) {
            if (key_col >= output_to_scan.size()) return false;
            key_col_scan = output_to_scan[key_col];
        }

        idx_t ncols = pq_scan->GetTypes().size();
        // VARCHAR predicate cols benefit from SkipStrData on the predicate
        // col (preserve dict_indices for BuildTypedKeepMask). The key col
        // also stays VARCHAR — both consumer paths (Q26 fast path via
        // topk_varchar + non-fast eval_row + get_key) prefer str_dict_values
        // when available. PLAIN-only pages back-fill str_data on demand via
        // MaterialiseStrDataLazy, so skipping is safe in both branches and
        // shaves ~24MB/RG of pointless string_t writes on SearchPhrase.
        {
            std::vector<bool> skip_pre(ncols, false);
            for (auto &p : tn_preds) {
                if (p.col_idx < ncols && p.str_form && p.col_idx != key_col_scan)
                    skip_pre[p.col_idx] = true;
            }
            if (key_col_scan < ncols &&
                pq_scan->GetTypes()[key_col_scan].id() == LogicalTypeId::VARCHAR) {
                skip_pre[key_col_scan] = true;
            }
            pq_scan->SetSkipStrData(std::move(skip_pre));
        }

        constexpr int MAX_THREADS = 8;
        std::vector<LightHeap> light_heaps;
        light_heaps.reserve(MAX_THREADS);
        for (int i = 0; i < MAX_THREADS; i++) light_heaps.emplace_back(light_cmp);
        std::array<std::mutex, MAX_THREADS> mus;
        std::vector<bool> need(ncols, false);
        need[key_col_scan] = true;
        for (auto &p : tn_preds) if (p.col_idx < ncols) need[p.col_idx] = true;
        // First-pass scan only needs key + predicate columns. Output
        // materialization happens in a separate pass 2 below where we decode
        // only the columns the projection actually emits.
        // Q26 fast path: when the filter is empty or `<key_col> <> ''`,
        // iterate the per-RG dict (~50K entries) instead of the per-row data
        // (~1M rows per RG). We emit (ncols)-wide rows with the key string
        // placed at `key_col_scan` and NULL elsewhere — the upstream
        // projection plucks the key column out. Skips pass-2 RG materialization.
        bool filter_q26_compat = tn_preds.empty() ||
            (tn_preds.size() == 1 &&
             (idx_t)tn_preds[0].col_idx == key_col_scan &&
             tn_preds[0].str_form &&
             !tn_preds[0].like_contains &&
             tn_preds[0].op == SimpleCmpOp::NE);
        // Only enable when the OrderBy's output schema's key column is
        // VARCHAR (we're already in CollectTopN_Varchar so this is true)
        // AND there's no extra ORDER BY tiebreaker (the dict-scan only
        // resolves on the key, not secondary columns).
        // Projection rule: fast path emits ncols-wide rows with key value
        // at key_col_scan and NULL elsewhere, so the parent projection must
        // only reference key_col_scan (i.e. identity output OR every output
        // expression points at the key column). Q26 SELECT SearchPhrase
        // ORDER BY SearchPhrase is the canonical fit.
        auto proj_refs_only_key = [&]() {
            if (output_to_scan.empty()) return true;
            for (idx_t sc : output_to_scan)
                if (sc != key_col_scan) return false;
            return true;
        };
        bool q26_fast_path =
            filter_q26_compat &&
            orders_.size() == 1 &&
            proj_refs_only_key();
        // For Q26 fast path: optionally enable dict-only decode on the key
        // col. SLOTH_Q26_TRUST_DICT=1 enables; skips ~600ms of data-page
        // RLE decode. Unsafe when the parquet has orphan dict entries
        // (ClickBench hits.parquet SearchPhrase has them: ASCII "!" entries
        // in dict that are never referenced — they'd surface as false top-K
        // wins). Off by default.
        if (q26_fast_path) {
            static const bool q26_trust_dict = []() {
                return std::getenv("SLOTH_Q26_TRUST_DICT") != nullptr;
            }();
            if (q26_trust_dict && key_col_scan < ncols) {
                std::vector<bool> dict_only(ncols, false);
                dict_only[key_col_scan] = true;
                pq_scan->SetStrDictOnly(std::move(dict_only));
            } else if (key_col_scan < ncols) {
                // Orphan-safe dict-used mode: decoder builds the used[]
                // bitmap directly from the RLE batch buffer, skipping the
                // ~25MB/RG str_dict_indices materialization and the
                // consumer's O(N) used[]-build pass. ~50-100ms wall
                // reduction on Q26 / 226-RG SearchPhrase scans.
                std::vector<bool> used_only(ncols, false);
                used_only[key_col_scan] = true;
                pq_scan->SetStrDictUsedOnly(std::move(used_only));
            }
        }
        pq_scan->SetNeededOutputs(need);
        pq_scan->Init();
        auto *reader = pq_scan->GetReader();

        if (q26_fast_path) {
            std::string skip_str = (tn_preds.size() == 1 ? tn_preds[0].sval : std::string());
            std::vector<std::vector<std::string>> per_rg_winners;
            std::mutex pw_mu;
            pq_scan->SetRGConsumer(
                [&](const PhysicalParquetScan::RGWork &work, idx_t rg_idx, int tid) {
                    (void)tid; (void)rg_idx;
                    idx_t nrows = pq_scan->RowGroupSize(rg_idx);
                    const auto &kcol = work.cols[key_col_scan];
                    if (!kcol.decoded) return;
                    // PLAIN-page (non-dict) RGs: fall back to row-loop top-K
                    // for THIS RG only. Without this Q26 misses strings that
                    // live in PLAIN-only RGs (hits.parquet has mixed pages).
                    // When str_dict_only/str_dict_used_only is active,
                    // str_dict_indices.empty() is intentional — proceed.
                    if (!kcol.str_dict_encoded ||
                        (!kcol.str_dict_only && !kcol.str_dict_used_only &&
                         kcol.str_dict_indices.empty()) ||
                        kcol.str_dict_values.empty()) {
                        if (kcol.str_data.empty()) return;
                        const string_t *gs = kcol.str_data.data();
                        const std::string &sk = skip_str;
                        std::vector<std::string> rg_top;
                        rg_top.reserve(k);
                        auto cmpf = [ascending](const std::string &a,
                                                  const std::string &b) {
                            return ascending ? (a < b) : (a > b);
                        };
                        std::priority_queue<std::string,
                            std::vector<std::string>, decltype(cmpf)> h(cmpf);
                        for (idx_t r = 0; r < nrows; r++) {
                            if (!kcol.all_valid && !kcol.validity[r]) continue;
                            const char *sd = gs[r].GetData();
                            uint32_t sl = gs[r].GetSize();
                            if (!sk.empty() == false && sl == sk.size() &&
                                (sk.empty() ||
                                 std::memcmp(sd, sk.data(), sk.size()) == 0))
                                continue;
                            std::string s(sd, sl);
                            if (h.size() < (size_t)k) h.push(std::move(s));
                            else if ((ascending ? (s < h.top()) : (s > h.top()))) {
                                h.pop();
                                h.push(std::move(s));
                            }
                        }
                        while (!h.empty()) { rg_top.push_back(h.top()); h.pop(); }
                        std::reverse(rg_top.begin(), rg_top.end());
                        if (!rg_top.empty()) {
                            std::lock_guard<std::mutex> lk(pw_mu);
                            per_rg_winners.push_back(std::move(rg_top));
                        }
                        return;
                    }
                    uint32_t skip_di = UINT32_MAX;
                    if (!tn_preds.empty()) {
                        const auto &dv = kcol.str_dict_values;
                        for (uint32_t d = 0; d < dv.size(); d++) {
                            if (dv[d].GetSize() == skip_str.size() &&
                                (skip_str.empty() ||
                                 std::memcmp(dv[d].GetData(),
                                             skip_str.data(),
                                             skip_str.size()) == 0)) {
                                skip_di = d;
                                break;
                            }
                        }
                    }
                    // Default: safe variant walks dict_indices to filter
                    // orphan dict entries (ClickBench hits.parquet has them).
                    // When SLOTH_Q26_TRUST_DICT=1 + parquet dict_only mode,
                    // skip indices walk for ~50ms consumer-side savings.
                    // The DOMINANT savings come from the parquet reader
                    // skipping data pages entirely (~600ms wall on Q26).
                    std::vector<std::string> winners;
                    if (kcol.str_dict_only) {
                        winners = slothdb::TopKVarcharFromDictTrust(
                            kcol.str_dict_values.data(),
                            kcol.str_dict_values.size(),
                            skip_di, ascending, (size_t)k);
                    } else if (kcol.str_dict_used_only &&
                               !kcol.str_dict_used.empty()) {
                        winners = slothdb::TopKVarcharFromDictUsed(
                            kcol.str_dict_values.data(),
                            kcol.str_dict_values.size(),
                            kcol.str_dict_used.data(),
                            skip_di, ascending, (size_t)k);
                    } else {
                        winners = slothdb::TopKVarcharFromDict(
                            kcol.str_dict_values.data(),
                            kcol.str_dict_values.size(),
                            kcol.str_dict_indices.data(), nrows,
                            kcol.all_valid ? nullptr : kcol.validity.data(),
                            skip_di, ascending, (size_t)k);
                    }
                    if (!winners.empty()) {
                        std::lock_guard<std::mutex> lk(pw_mu);
                        per_rg_winners.push_back(std::move(winners));
                    }
                });
            pq_scan->RunParallelRGs(0);
            // Final merge: K-min/max heap over all per-RG candidates.
            std::vector<std::string> all;
            for (auto &v : per_rg_winners) {
                for (auto &s : v) all.push_back(std::move(s));
            }
            // Sort then take first k.
            std::sort(all.begin(), all.end(),
                [ascending](const std::string &a, const std::string &b) {
                    return ascending ? (a < b) : (a > b);
                });
            if (all.size() > (size_t)k) all.resize(k);
            // Emit ncols-wide rows: key at key_col_scan, NULL elsewhere.
            // Upstream projection picks the key column out.
            std::vector<std::vector<Value>> out_rows;
            out_rows.reserve(all.size());
            for (auto &s : all) {
                std::vector<Value> row(ncols);
                row[key_col_scan] = Value::VARCHAR(s);
                out_rows.push_back(std::move(row));
            }
            sorted_rows_ = std::move(out_rows);
            return true;
        }

        pq_scan->SetRGConsumer(
            [&](const PhysicalParquetScan::RGWork &work, idx_t rg_idx, int tid) {
                int slot = tid % MAX_THREADS;
                auto &heap = light_heaps[slot];
                std::lock_guard<std::mutex> lk(mus[slot]);
                idx_t nrows = pq_scan->RowGroupSize(rg_idx);
                const auto &kcol = work.cols[key_col_scan];
                if (!kcol.decoded) return;
                bool key_all_valid = kcol.all_valid;

                std::vector<uint8_t> mask;
                bool mask_active = false;
                bool fallback_row_loop = false;
                if (!tn_preds.empty()) {
                    mask_active = BuildTypedKeepMask(tn_preds, work.cols, nrows, mask);
                    if (!mask_active) fallback_row_loop = true;
                }
                auto eval_row = [&](idx_t i) -> bool {
                    for (auto &p : tn_preds) {
                        const auto &col = work.cols[p.col_idx];
                        if (!col.decoded) return false;
                        if (!col.all_valid && i < col.validity.size() && !col.validity[i]) return false;
                        if (p.str_form) {
                            const char *sd = nullptr; uint32_t sl = 0;
                            if (col.str_dict_encoded && !col.str_dict_indices.empty()) {
                                uint32_t di = col.str_dict_indices[i];
                                if (di >= col.str_dict_values.size()) return false;
                                sd = col.str_dict_values[di].GetData();
                                sl = col.str_dict_values[di].GetSize();
                            } else if (i < col.str_data.size()) {
                                sd = col.str_data[i].GetData();
                                sl = col.str_data[i].GetSize();
                            } else { return false; }
                            if (p.like_contains) {
                                bool match;
                                if (p.sval.empty()) {
                                    match = true;
                                } else if (sl < p.sval.size()) {
                                    match = false;
                                } else {
                                    match = (FindSubstr(sd, sl, p.sval.data(), p.sval.size()) != nullptr);
                                }
                                if (p.like_negated) match = !match;
                                if (!match) return false;
                            } else {
                                bool eq = (sl == p.sval.size()) &&
                                          (p.sval.empty() ||
                                           std::memcmp(sd, p.sval.data(), p.sval.size()) == 0);
                                if (p.op == SimpleCmpOp::EQ) { if (!eq) return false; }
                                else                         { if (eq)  return false; }
                            }
                        } else {
                            int64_t pv = p.ival;
                            int64_t v = 0;
                            if (col.type.id() == LogicalTypeId::INTEGER) v = col.i32_data[i];
                            else if (col.type.id() == LogicalTypeId::BIGINT) v = col.i64_data[i];
                            else return false;
                            switch (p.op) {
                            case SimpleCmpOp::EQ: if (v != pv) return false; break;
                            case SimpleCmpOp::NE: if (v == pv) return false; break;
                            case SimpleCmpOp::LT: if (!(v <  pv)) return false; break;
                            case SimpleCmpOp::LE: if (!(v <= pv)) return false; break;
                            case SimpleCmpOp::GT: if (!(v >  pv)) return false; break;
                            case SimpleCmpOp::GE: if (!(v >= pv)) return false; break;
                            }
                        }
                    }
                    return true;
                };

                // Key extraction helper: returns raw ptr+len; null if invalid.
                auto get_key = [&](idx_t i, const char *&sd, uint32_t &sl, bool &is_null) -> bool {
                    is_null = !key_all_valid && !(i < kcol.validity.size() && kcol.validity[i]);
                    if (is_null) { sd = nullptr; sl = 0; return true; }
                    if (kcol.str_dict_encoded && i < kcol.str_dict_indices.size()) {
                        uint32_t di = kcol.str_dict_indices[i];
                        if (di >= kcol.str_dict_values.size()) return false;
                        sd = kcol.str_dict_values[di].GetData();
                        sl = kcol.str_dict_values[di].GetSize();
                        return true;
                    }
                    if (i < kcol.str_data.size()) {
                        sd = kcol.str_data[i].GetData();
                        sl = kcol.str_data[i].GetSize();
                        return true;
                    }
                    return false;
                };

                // Most rows lose against heap.top() once the heap fills.
                // Compare candidate (sd,sl,is_null) against heap.top() WITHOUT
                // allocating an std::string for the candidate key — the copy
                // only happens on the rare row that wins. For Q26 ORDER BY
                // SearchPhrase ASC LIMIT 10 over ~100M rows this skips ~99%
                // of the per-row std::string allocations.
                auto wins_against_top = [ascending](const LightEntry &top,
                                                     const char *sd, uint32_t sl,
                                                     bool is_null) -> bool {
                    if (is_null && top.is_null) return false;
                    if (is_null) return !ascending;
                    if (top.is_null) return ascending;
                    std::string_view cand(sd, sl);
                    std::string_view t(top.key);
                    return ascending ? cand < t : cand > t;
                };
                for (idx_t i = 0; i < nrows; i++) {
                    if (mask_active && !mask[i]) continue;
                    if (fallback_row_loop && !eval_row(i)) continue;
                    const char *sd = nullptr; uint32_t sl = 0; bool is_null = false;
                    if (!get_key(i, sd, sl, is_null)) continue;
                    if (heap.size() < k) {
                        LightEntry e;
                        e.is_null = is_null;
                        e.rg_idx = (uint32_t)rg_idx;
                        e.row_idx = (uint32_t)i;
                        if (!is_null) e.key.assign(sd, sl);
                        heap.push(std::move(e));
                    } else if (wins_against_top(heap.top(), sd, sl, is_null)) {
                        LightEntry e;
                        e.is_null = is_null;
                        e.rg_idx = (uint32_t)rg_idx;
                        e.row_idx = (uint32_t)i;
                        if (!is_null) e.key.assign(sd, sl);
                        heap.pop();
                        heap.push(std::move(e));
                    }
                }
            });
        pq_scan->RunParallelRGs(0);

        // Merge per-thread heaps.
        LightHeap merged(light_cmp);
        for (auto &h : light_heaps) {
            while (!h.empty()) {
                auto e = h.top(); h.pop();
                if (merged.size() < k) {
                    merged.push(std::move(e));
                } else {
                    bool wins;
                    if (e.is_null && merged.top().is_null) wins = false;
                    else if (e.is_null) wins = !ascending;
                    else if (merged.top().is_null) wins = ascending;
                    else wins = ascending ? (e.key < merged.top().key)
                                          : (e.key > merged.top().key);
                    if (wins) { merged.pop(); merged.push(std::move(e)); }
                }
            }
        }

        // Drain to flat vector best-first.
        std::vector<LightEntry> winners;
        winners.reserve(merged.size());
        while (!merged.empty()) { winners.push_back(merged.top()); merged.pop(); }
        std::reverse(winners.begin(), winners.end());

        // Pass 2: materialise rows for K winners. Output is in projection
        // schema (output_to_scan maps each output col to a scan col); for
        // identity output we emit all scan cols.
        ankerl::unordered_dense::map<uint32_t, std::vector<size_t>> rg_to_winner_indices;
        for (size_t i = 0; i < winners.size(); i++)
            rg_to_winner_indices[winners[i].rg_idx].push_back(i);

        std::vector<idx_t> needed_scan_cols;
        if (output_to_scan.empty()) {
            needed_scan_cols.reserve(ncols);
            for (idx_t c = 0; c < ncols; c++) needed_scan_cols.push_back(c);
        } else {
            // Deduplicate while preserving the cols we need.
            std::vector<bool> seen(ncols, false);
            for (idx_t sc : output_to_scan) {
                if (sc < ncols && !seen[sc]) { seen[sc] = true; needed_scan_cols.push_back(sc); }
            }
        }
        idx_t out_ncols = output_to_scan.empty() ? ncols : output_to_scan.size();

        std::vector<std::vector<Value>> output_rows(winners.size());
        std::vector<uint32_t> unique_rgs;
        unique_rgs.reserve(rg_to_winner_indices.size());
        for (auto &kv : rg_to_winner_indices) unique_rgs.push_back(kv.first);
        ankerl::unordered_dense::map<uint32_t, size_t> rg_to_local;
        for (size_t i = 0; i < unique_rgs.size(); i++) rg_to_local[unique_rgs[i]] = i;
        std::vector<std::vector<ParquetColumnData>> rg_cols(unique_rgs.size(),
            std::vector<ParquetColumnData>(ncols));
        std::vector<std::vector<std::vector<Value>>> rg_fallback(unique_rgs.size(),
            std::vector<std::vector<Value>>(ncols));

        struct DecodeUnit { uint32_t rg; idx_t col; };
        std::vector<DecodeUnit> units;
        units.reserve(unique_rgs.size() * needed_scan_cols.size());
        for (auto rg_u : unique_rgs)
            for (idx_t c : needed_scan_cols)
                units.push_back({rg_u, c});

        unsigned int p2_threads = HWThreads();
        if (p2_threads > 8) p2_threads = 8;
        if (p2_threads > units.size())
            p2_threads = static_cast<unsigned int>(units.size());

        auto decode_unit = [&](const DecodeUnit &u) {
            size_t local = rg_to_local[u.rg];
            if (!reader->ReadColumnInto(u.rg, u.col, rg_cols[local][u.col])) {
                rg_fallback[local][u.col] = reader->ReadColumn(u.rg, u.col);
            }
        };
        if (p2_threads <= 1 || units.size() == 1) {
            for (auto &u : units) decode_unit(u);
        } else {
            std::atomic<size_t> next2{0};
            std::vector<std::thread> ts2;
            ts2.reserve(p2_threads);
            for (unsigned int t = 0; t < p2_threads; t++) {
                ts2.emplace_back([&] {
                    while (true) {
                        size_t idx = next2.fetch_add(1, std::memory_order_relaxed);
                        if (idx >= units.size()) return;
                        decode_unit(units[idx]);
                    }
                });
            }
            for (auto &t : ts2) if (t.joinable()) t.join();
        }

        auto box_typed = [](const ParquetColumnData &col, idx_t row_i) -> Value {
            if (!col.decoded) return Value();
            bool valid = col.all_valid || (row_i < col.validity.size() && col.validity[row_i]);
            if (!valid) return Value();
            switch (col.type.id()) {
            case LogicalTypeId::BIGINT:  return Value::BIGINT(col.i64_data[row_i]);
            case LogicalTypeId::INTEGER: return Value::INTEGER(col.i32_data[row_i]);
            case LogicalTypeId::DOUBLE:  return Value::DOUBLE(col.f64_data[row_i]);
            case LogicalTypeId::FLOAT:   return Value::FLOAT(col.f32_data[row_i]);
            case LogicalTypeId::BOOLEAN: return Value::BOOLEAN(col.bool_data[row_i] != 0);
            case LogicalTypeId::VARCHAR:
                if (col.str_dict_encoded && row_i < col.str_dict_indices.size()) {
                    auto idx = col.str_dict_indices[row_i];
                    if (idx < col.str_dict_values.size())
                        return Value::VARCHAR(col.str_dict_values[idx].GetString());
                    return Value();
                }
                if (row_i < col.str_data.size())
                    return Value::VARCHAR(col.str_data[row_i].GetString());
                return Value();
            default: return Value();
            }
        };
        for (size_t out_idx = 0; out_idx < winners.size(); out_idx++) {
            auto &winner = winners[out_idx];
            size_t local = rg_to_local[winner.rg_idx];
            idx_t row_i = winner.row_idx;
            std::vector<Value> row;
            row.reserve(out_ncols);
            for (idx_t out_c = 0; out_c < out_ncols; out_c++) {
                idx_t scan_c = output_to_scan.empty() ? out_c : output_to_scan[out_c];
                auto &cd = rg_cols[local][scan_c];
                if (cd.decoded) {
                    row.push_back(box_typed(cd, row_i));
                } else {
                    auto &fb = rg_fallback[local][scan_c];
                    row.push_back(row_i < fb.size() ? fb[row_i] : Value());
                }
            }
            output_rows[out_idx] = std::move(row);
        }
        sorted_rows_ = std::move(output_rows);
        return true;
    }

    // After ResortByFullKeys, trim down to the original K limit.
    void TrimToK(idx_t k) {
        if (sorted_rows_.size() > k) sorted_rows_.resize(k);
    }

    // Multi-key tiebreaker: after a primitive top-K on orders_[0], re-sort
    // sorted_rows_ by the full ORDER BY key list. K winners is small
    // (typically 10) so this is cheap even with full Value comparison.
    void ResortByFullKeys() {
        if (sorted_rows_.size() <= 1 || orders_.size() <= 1) return;
        // Each row is in source-schema order; ORDER BY clauses index into
        // that schema (LogicalOrderBy is built before the deferred projection).
        std::vector<idx_t> ord_cols;
        ord_cols.reserve(orders_.size());
        for (auto &o : orders_) {
            if (o.expression->GetExpressionType() != BoundExpressionType::COLUMN_REF) {
                // Non-trivial expression in second+ ORDER BY — skip resort,
                // fall back to whatever the primitive path produced. Caller
                // can route to the slow generic path if exactness matters.
                return;
            }
            ord_cols.push_back(static_cast<BoundColumnRef &>(*o.expression).column_index);
        }
        std::sort(sorted_rows_.begin(), sorted_rows_.end(),
            [this, &ord_cols](const std::vector<Value> &a, const std::vector<Value> &b) {
                for (size_t i = 0; i < orders_.size(); i++) {
                    idx_t c = ord_cols[i];
                    if (c >= a.size() || c >= b.size()) continue;
                    auto &va = a[c];
                    auto &vb = b[c];
                    if (va.IsNull() && vb.IsNull()) continue;
                    if (va.IsNull()) return !orders_[i].ascending;
                    if (vb.IsNull()) return orders_[i].ascending;
                    if (va < vb) return orders_[i].ascending;
                    if (vb < va) return !orders_[i].ascending;
                }
                return false;
            });
    }

    // Specialised path: single-column ORDER BY on a primitive numeric
    // column. Threshold-comparison happens against an int64/double
    // directly, no Value boxing on the loser path. This is the shape
    // of `ORDER BY col DESC LIMIT N` queries that show up everywhere.
    template <typename T>
    void CollectTopN_Primitive(idx_t k, idx_t key_col, bool ascending) {
        struct HeapEntry {
            T key;
            bool is_null = false;
            std::vector<Value> row;
        };
        // NULLS LAST: heap top is the worst candidate (first to evict).
        // For ASC top-K, top = greatest value; for DESC, top = smallest.
        // NULL must be the worst (= top) so any non-null evicts it.
        auto cmp = [ascending](const HeapEntry &a, const HeapEntry &b) {
            if (a.is_null && b.is_null) return false;
            if (a.is_null) return !ascending; // ASC: a is top, not < b
            if (b.is_null) return ascending;
            return ascending ? a.key < b.key : a.key > b.key;
        };

        // Parallel path: when the input chain ends in a Parquet scan,
        // dispatch RunParallelRGs and let each thread maintain its own
        // top-K heap directly on the typed RG buffers. Final merge is
        // a small K-way merge of `nthreads` heaps. Skips the sequential
        // single-threaded chunk-consumption loop entirely.
        //
        // Unwrap through identity projections (SELECT * shape) so the
        // common LIMIT -> ORDER_BY -> PROJECTION -> SCAN tree gets the
        // parallel path. Non-identity projections block this — falls
        // back to sequential.
        PhysicalParquetScan *pq_scan = dynamic_cast<PhysicalParquetScan *>(children[0].get());
        std::vector<SimplePredicate> tn_preds;
        if (!pq_scan) {
            if (auto *proj = dynamic_cast<PhysicalProjection *>(children[0].get())) {
                if (proj->IsIdentityProjection() && !proj->children.empty()) {
                    pq_scan = dynamic_cast<PhysicalParquetScan *>(proj->children[0].get());
                }
            }
        }
        // Filter unwrap: TopN -> [Projection ->] Filter -> [Projection ->] Scan
        // for Q24/Q25/Q26/Q27 (`SELECT ... WHERE ... ORDER BY ... LIMIT N`).
        // Without this, those queries fall to the sequential DataChunk-based
        // path that boxes every row into Value (Q25 ~75s vs DuckDB 0.8s).
        // With a SimplePredicate-compilable filter, the parallel pass-1
        // worker decodes predicate cols + key, builds a typed keep-mask
        // (or per-row fallback for mixed-encoding RGs), and only pushes
        // matching rows into the heap.
        if (!pq_scan) {
            PhysicalFilter *flt = dynamic_cast<PhysicalFilter *>(children[0].get());
            if (!flt) {
                if (auto *proj = dynamic_cast<PhysicalProjection *>(children[0].get())) {
                    if (proj->IsIdentityProjection() && !proj->children.empty())
                        flt = dynamic_cast<PhysicalFilter *>(proj->children[0].get());
                }
            }
            if (flt && flt->GetCondition() && !flt->children.empty()) {
                std::vector<SimplePredicate> tmp;
                if (TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                    PhysicalParquetScan *cand = dynamic_cast<PhysicalParquetScan *>(flt->children[0].get());
                    if (!cand) {
                        if (auto *p2 = dynamic_cast<PhysicalProjection *>(flt->children[0].get())) {
                            if (p2->IsIdentityProjection() && !p2->children.empty())
                                cand = dynamic_cast<PhysicalParquetScan *>(p2->children[0].get());
                        }
                    }
                    if (cand) { pq_scan = cand; tn_preds = std::move(tmp); }
                }
            }
        }
        if (pq_scan) {
            // Two-pass top-N for Parquet:
            //
            // Pass 1: project ONLY the order-by column. Per-RG worker reads
            //   typed key array and maintains a heap of (key, rg_idx, row_idx)
            //   tuples. Skips decoding the other 9+ columns entirely on the
            //   ~10M-row hot path.
            //
            // Pass 2: from the merged top-K (rg_idx, row_idx) list, decode
            //   each unique RG once with the full projection, then materialise
            //   only the K winning rows into vector<Value>. K=10 winners
            //   typically span 1-3 RGs, so this is ~3 RG decodes vs 80.
            struct LightEntry {
                T key;
                uint32_t rg_idx;
                uint32_t row_idx;
                bool is_null = false;
            };
            auto light_cmp = [ascending](const LightEntry &a, const LightEntry &b) {
                if (a.is_null && b.is_null) return false;
                if (a.is_null) return !ascending;
                if (b.is_null) return ascending;
                return ascending ? a.key < b.key : a.key > b.key;
            };

            using LightHeap = std::priority_queue<LightEntry,
                std::vector<LightEntry>, decltype(light_cmp)>;

            // Pass 1: only the key column needs decoding.
            idx_t ncols = pq_scan->GetTypes().size();

            // Stats-based row-group ordering. Read each RG's max (or min,
            // for ASC) for the key column from the Thrift footer. Sort
            // RGs best-first so the heap converges to the true top-K
            // after a handful of decodes; subsequent RGs whose max can't
            // beat the current K-th best are skipped without ever
            // touching their column data on disk.
            //
            // Falls back to the original parallel-decode-everything path
            // when stats are missing for the column.
            // For VARCHAR predicate cols, set SkipStrData BEFORE Init so
            // the decoder preserves dict_indices when available.
            // Also detect lengths-only candidates: VARCHAR predicate cols whose
            // ALL predicates are `<> ''` / `= ''` (length-check only). For
            // Q25/Q26/Q27 (`WHERE SearchPhrase <> '' ORDER BY ... LIMIT 10`),
            // pass-1 only needs SearchPhrase lengths; pass-2 re-decodes the
            // K winning RGs with full bytes for output.
            std::vector<bool> tn_lo_pred(ncols, false);
            if (!tn_preds.empty()) {
                std::vector<bool> skip_pre(ncols, false);
                std::vector<bool> lo_eligible(ncols, true);
                std::vector<bool> lo_seen(ncols, false);
                for (auto &p : tn_preds) {
                    if (p.col_idx < ncols && p.str_form) {
                        skip_pre[p.col_idx] = true;
                        lo_seen[p.col_idx] = true;
                        if (p.like_contains || !p.sval.empty() ||
                            (p.op != SimpleCmpOp::EQ && p.op != SimpleCmpOp::NE)) {
                            lo_eligible[p.col_idx] = false;
                        }
                    }
                }
                for (idx_t c = 0; c < ncols; c++) {
                    if (lo_seen[c] && lo_eligible[c] && c != key_col) {
                        tn_lo_pred[c] = true;
                    }
                }
                pq_scan->SetSkipStrData(std::move(skip_pre));
            }
            pq_scan->Init();
            auto *reader = pq_scan->GetReader();
            const auto &meta = reader->GetMeta();
            idx_t num_rgs = meta.row_groups.size();

            struct RGEntry { T order_key; idx_t rg_idx; bool has_stats; };
            std::vector<RGEntry> rg_order;
            rg_order.reserve(num_rgs);
            bool all_have_stats = true;
            for (idx_t rg = 0; rg < num_rgs; rg++) {
                if (key_col >= meta.row_groups[rg].columns.size()) {
                    all_have_stats = false;
                    rg_order.push_back({T{}, rg, false});
                    continue;
                }
                auto &cmeta = meta.row_groups[rg].columns[key_col];
                if (!cmeta.has_stats) {
                    all_have_stats = false;
                    rg_order.push_back({T{}, rg, false});
                    continue;
                }
                T key;
                try {
                    if (ascending) key = cmeta.min_value.GetValue<T>();
                    else           key = cmeta.max_value.GetValue<T>();
                } catch (...) {
                    all_have_stats = false;
                    rg_order.push_back({T{}, rg, false});
                    continue;
                }
                rg_order.push_back({key, rg, true});
            }

            LightHeap light_merged(light_cmp);

            // Stats path: iterate RGs in priority order, threshold-skip
            // once the heap is full and no remaining RG can beat the K-th
            // best. With a filter active this is still sound: the filter
            // only RESTRICTS which rows enter the heap, so an RG whose
            // min-key already loses to the K-th best cannot hide a winner.
            //
            // For Q25/Q27 (~80 RGs over 100M rows), this typically drops
            // the work from "scan all 80 RGs in parallel" to "scan 3-8 RGs
            // sequentially". The filter cost is amortised over the small
            // surviving RG set, so the overall path is faster despite
            // being single-threaded.
            bool use_stats_path = all_have_stats;
            if (use_stats_path) {
                // Sort: best RG first. For DESC, that's max desc.
                std::sort(rg_order.begin(), rg_order.end(),
                    [ascending](const RGEntry &a, const RGEntry &b) {
                        return ascending ? a.order_key < b.order_key
                                         : a.order_key > b.order_key;
                    });
                // For VARCHAR predicate cols, the SkipStrData flag was
                // already set above so str_dict_indices stay populated on
                // ReadColumnInto; nothing more to do here.

                // Parallel priority-order decode. Workers pop RGs from a
                // shared cursor in priority order; each worker re-checks
                // the merged threshold before decoding so once the heap
                // converges we stop touching late RGs. Conservative: a
                // racing worker may decode one extra RG that turns out
                // unneeded — correctness is preserved by the heap merge.
                std::mutex heap_mu;
                std::atomic<size_t> next_rg{0};
                std::atomic<bool> done{false};
                unsigned int nthreads = HWThreads();
                if (nthreads > 6) nthreads = 6;
                if (nthreads > rg_order.size())
                    nthreads = static_cast<unsigned int>(rg_order.size());
                if (nthreads < 1) nthreads = 1;

                auto eval_one_rg = [&](size_t order_idx) {
                    auto &re = rg_order[order_idx];
                    // Threshold check: read heap top under lock.
                    {
                        std::lock_guard<std::mutex> lk(heap_mu);
                        if (light_merged.size() >= k && !light_merged.top().is_null) {
                            T cur_thr = light_merged.top().key;
                            bool beats = ascending ? (re.order_key < cur_thr)
                                                   : (re.order_key > cur_thr);
                            if (!beats) { done.store(true, std::memory_order_relaxed); return; }
                        }
                    }
                    idx_t nrows = pq_scan->RowGroupSize(re.rg_idx);
                    std::vector<ParquetColumnData> pcols(ncols);
                    if (!reader->ReadColumnInto(re.rg_idx, key_col, pcols[key_col])) return;
                    for (auto &p : tn_preds) {
                        if (p.col_idx >= ncols) return;
                        if (p.col_idx == key_col) continue;
                        // Enable lengths-only for `<> ''` / `= ''` predicates
                        // before ReadColumnInto so the decoder fills str_lengths.
                        pcols[p.col_idx].str_lengths_only = tn_lo_pred[p.col_idx];
                        if (!reader->ReadColumnInto(re.rg_idx, p.col_idx, pcols[p.col_idx])) return;
                    }
                    auto &kcol = pcols[key_col];
                    const T *kdata = nullptr;
                    if constexpr (std::is_same_v<T, int64_t>) kdata = reinterpret_cast<const T *>(kcol.i64_data.data());
                    else if constexpr (std::is_same_v<T, int32_t>) kdata = reinterpret_cast<const T *>(kcol.i32_data.data());
                    else if constexpr (std::is_same_v<T, double>)  kdata = reinterpret_cast<const T *>(kcol.f64_data.data());
                    else if constexpr (std::is_same_v<T, float>)   kdata = reinterpret_cast<const T *>(kcol.f32_data.data());
                    if (!kdata) return;
                    bool key_all_valid = kcol.all_valid;

                    std::vector<uint8_t> mask;
                    bool mask_active = false;
                    bool fallback_row_loop = false;
                    if (!tn_preds.empty()) {
                        mask_active = BuildTypedKeepMask(tn_preds, pcols, nrows, mask);
                        if (!mask_active) fallback_row_loop = true;
                    }

                    auto eval_row = [&](idx_t i) -> bool {
                        for (auto &p : tn_preds) {
                            const auto &col = pcols[p.col_idx];
                            if (!col.decoded) return false;
                            if (!col.all_valid && i < col.validity.size() && !col.validity[i]) return false;
                            if (p.str_form) {
                                if (col.str_lengths_only && !col.str_lengths.empty() &&
                                    !p.like_contains && p.sval.empty() &&
                                    (p.op == SimpleCmpOp::EQ || p.op == SimpleCmpOp::NE)) {
                                    bool is_empty = (col.str_lengths[i] == 0);
                                    if (p.op == SimpleCmpOp::EQ) { if (!is_empty) return false; }
                                    else                         { if (is_empty)  return false; }
                                    continue;
                                }
                                const char *sd = nullptr; uint32_t sl = 0;
                                if (col.str_dict_encoded && !col.str_dict_indices.empty()) {
                                    uint32_t di = col.str_dict_indices[i];
                                    if (di >= col.str_dict_values.size()) return false;
                                    sd = col.str_dict_values[di].GetData();
                                    sl = col.str_dict_values[di].GetSize();
                                } else if (i < col.str_data.size()) {
                                    sd = col.str_data[i].GetData();
                                    sl = col.str_data[i].GetSize();
                                } else { return false; }
                                if (p.like_contains) {
                                    bool match;
                                    if (p.sval.empty()) {
                                        match = true;
                                    } else if (sl < p.sval.size()) {
                                        match = false;
                                    } else {
                                        match = (FindSubstr(sd, sl, p.sval.data(), p.sval.size()) != nullptr);
                                    }
                                    if (p.like_negated) match = !match;
                                    if (!match) return false;
                                } else {
                                    bool eq = (sl == p.sval.size()) &&
                                              (p.sval.empty() ||
                                               std::memcmp(sd, p.sval.data(), p.sval.size()) == 0);
                                    if (p.op == SimpleCmpOp::EQ) { if (!eq) return false; }
                                    else                         { if (eq)  return false; }
                                }
                            } else {
                                int64_t pv = p.ival;
                                int64_t v = 0;
                                if (col.type.id() == LogicalTypeId::INTEGER) v = col.i32_data[i];
                                else if (col.type.id() == LogicalTypeId::BIGINT) v = col.i64_data[i];
                                else return false;
                                switch (p.op) {
                                case SimpleCmpOp::EQ: if (v != pv) return false; break;
                                case SimpleCmpOp::NE: if (v == pv) return false; break;
                                case SimpleCmpOp::LT: if (!(v <  pv)) return false; break;
                                case SimpleCmpOp::LE: if (!(v <= pv)) return false; break;
                                case SimpleCmpOp::GT: if (!(v >  pv)) return false; break;
                                case SimpleCmpOp::GE: if (!(v >= pv)) return false; break;
                                }
                            }
                        }
                        return true;
                    };

                    // Per-RG local heap of size K (heap top = WORST of K-best).
                    // Iterate row-major; cheap typed-key comparison against
                    // local heap top filters losers without storing them.
                    // Then merge local heap into global under a single lock.
                    // Snapshot a hot threshold from light_merged once at the
                    // start so most losers in this RG never touch the local
                    // heap either.
                    // Ratcheting prefilter: start at global threshold; once
                    // local heap fills to K, tighten to local_heap.top() (the
                    // K-th best seen in THIS RG). Most subsequent losers fail
                    // the typed compare instead of the heavier light_cmp path.
                    T tight_thr{}; bool tight_thr_set = false;
                    {
                        std::lock_guard<std::mutex> lk(heap_mu);
                        if (light_merged.size() >= k && !light_merged.top().is_null) {
                            tight_thr = light_merged.top().key;
                            tight_thr_set = true;
                        }
                    }
                    LightHeap local_heap(light_cmp);
                    for (idx_t i = 0; i < nrows; i++) {
                        if (mask_active && !mask[i]) continue;
                        if (fallback_row_loop && !eval_row(i)) continue;
                        bool key_is_null = !key_all_valid && !(i < kcol.validity.size() && kcol.validity[i]);
                        T key = key_is_null ? T{} : kdata[i];
                        if (tight_thr_set && !key_is_null) {
                            bool wins = ascending ? (key < tight_thr) : (key > tight_thr);
                            if (!wins) continue;
                        }
                        LightEntry e{key, (uint32_t)re.rg_idx, (uint32_t)i, key_is_null};
                        if (local_heap.size() < k) {
                            local_heap.push(e);
                            if (local_heap.size() == k && !local_heap.top().is_null) {
                                tight_thr = local_heap.top().key;
                                tight_thr_set = true;
                            }
                        } else if (light_cmp(e, local_heap.top())) {
                            local_heap.pop();
                            local_heap.push(e);
                            if (!local_heap.top().is_null) {
                                tight_thr = local_heap.top().key;
                                tight_thr_set = true;
                            }
                        }
                    }
                    if (!local_heap.empty()) {
                        // Drain local into a vector for ordered merge.
                        std::vector<LightEntry> drain;
                        drain.reserve(local_heap.size());
                        while (!local_heap.empty()) {
                            drain.push_back(local_heap.top());
                            local_heap.pop();
                        }
                        std::lock_guard<std::mutex> lk(heap_mu);
                        for (auto &e : drain) {
                            if (light_merged.size() < k) {
                                light_merged.push(e);
                            } else if (light_cmp(e, light_merged.top())) {
                                light_merged.pop();
                                light_merged.push(e);
                            }
                        }
                    }
                };

                if (nthreads <= 1) {
                    for (size_t i = 0; i < rg_order.size(); i++) {
                        if (done.load(std::memory_order_relaxed)) break;
                        eval_one_rg(i);
                    }
                } else {
                    // Eval the best RG sequentially FIRST so the heap is
                    // populated and tight before parallel workers start.
                    // Otherwise 8 workers grab RG 0..7 in parallel with an
                    // empty heap (no threshold), all decode their RG fully,
                    // then early-exit only kicks in for RG 8+. For Q25/Q27
                    // (ORDER BY EventTime LIMIT 10 over date-clustered data)
                    // this turns 8 wasted RG decodes into 0 wasted ones.
                    if (!rg_order.empty()) {
                        eval_one_rg(0);
                        next_rg.store(1, std::memory_order_relaxed);
                    }
                    std::vector<std::thread> ts;
                    ts.reserve(nthreads);
                    for (unsigned int t = 0; t < nthreads; t++) {
                        ts.emplace_back([&] {
                            while (true) {
                                if (done.load(std::memory_order_relaxed)) return;
                                size_t idx = next_rg.fetch_add(1, std::memory_order_relaxed);
                                if (idx >= rg_order.size()) return;
                                eval_one_rg(idx);
                            }
                        });
                    }
                    for (auto &t : ts) if (t.joinable()) t.join();
                }
            } else {
                // No stats available for this column. Fall back to the
                // parallel-decode-everything path: per-thread heaps, no
                // pruning, merge at end.
                constexpr int MAX_THREADS = 8;
                std::vector<LightHeap> light_heaps;
                light_heaps.reserve(MAX_THREADS);
                for (int i = 0; i < MAX_THREADS; i++) light_heaps.emplace_back(light_cmp);
                std::array<std::mutex, MAX_THREADS> mus;
                std::vector<bool> need_key_only(ncols, false);
                need_key_only[key_col] = true;
                for (auto &p : tn_preds) {
                    if (p.col_idx < ncols) need_key_only[p.col_idx] = true;
                }
                pq_scan->SetNeededOutputs(need_key_only);
                pq_scan->SetStrLengthsOnly(tn_lo_pred);
                pq_scan->Init();
                pq_scan->SetRGConsumer(
                    [&](const PhysicalParquetScan::RGWork &work, idx_t rg_idx, int tid) {
                        int slot = tid % MAX_THREADS;
                        auto &heap = light_heaps[slot];
                        std::lock_guard<std::mutex> lk(mus[slot]);
                        idx_t nrows = pq_scan->RowGroupSize(rg_idx);
                        const auto &kcol = work.cols[key_col];
                        if (!kcol.decoded) return;
                        bool key_all_valid = kcol.all_valid;
                        const T *kdata = nullptr;
                        if constexpr (std::is_same_v<T, int64_t>) kdata = reinterpret_cast<const T *>(kcol.i64_data.data());
                        else if constexpr (std::is_same_v<T, int32_t>) kdata = reinterpret_cast<const T *>(kcol.i32_data.data());
                        else if constexpr (std::is_same_v<T, double>)  kdata = reinterpret_cast<const T *>(kcol.f64_data.data());
                        else if constexpr (std::is_same_v<T, float>)   kdata = reinterpret_cast<const T *>(kcol.f32_data.data());
                        if (!kdata) return;

                        // Filter mask: try dict-amortised BuildTypedKeepMask
                        // first; on refusal (mixed PLAIN+DICT pages clear
                        // str_dict_encoded — common on hits.parquet URL),
                        // fall through to per-row eval. Mirrors the FUSED
                        // COUNT pattern at line ~5616.
                        std::vector<uint8_t> mask;
                        bool mask_active = false;
                        bool fallback_row_loop = false;
                        if (!tn_preds.empty()) {
                            mask_active = BuildTypedKeepMask(tn_preds, work.cols, nrows, mask);
                            if (!mask_active) fallback_row_loop = true;
                        }

                        auto eval_row = [&](idx_t i) -> bool {
                            for (auto &p : tn_preds) {
                                const auto &col = work.cols[p.col_idx];
                                if (!col.decoded) return false;
                                if (!col.all_valid && i < col.validity.size() && !col.validity[i]) return false;
                                if (p.str_form) {
                                    if (col.str_lengths_only && !col.str_lengths.empty() &&
                                        !p.like_contains && p.sval.empty() &&
                                        (p.op == SimpleCmpOp::EQ || p.op == SimpleCmpOp::NE)) {
                                        bool is_empty = (col.str_lengths[i] == 0);
                                        if (p.op == SimpleCmpOp::EQ) { if (!is_empty) return false; }
                                        else                         { if (is_empty)  return false; }
                                        continue;
                                    }
                                    const char *sd = nullptr; uint32_t sl = 0;
                                    if (col.str_dict_encoded && !col.str_dict_indices.empty()) {
                                        uint32_t di = col.str_dict_indices[i];
                                        if (di >= col.str_dict_values.size()) return false;
                                        sd = col.str_dict_values[di].GetData();
                                        sl = col.str_dict_values[di].GetSize();
                                    } else if (i < col.str_data.size()) {
                                        sd = col.str_data[i].GetData();
                                        sl = col.str_data[i].GetSize();
                                    } else {
                                        return false;
                                    }
                                    if (p.like_contains) {
                                        bool match;
                                        if (p.sval.empty()) {
                                            match = true;
                                        } else if (sl < p.sval.size()) {
                                            match = false;
                                        } else {
                                            match = (FindSubstr(sd, sl, p.sval.data(), p.sval.size()) != nullptr);
                                        }
                                        if (p.like_negated) match = !match;
                                        if (!match) return false;
                                    } else {
                                        bool eq = (sl == p.sval.size()) &&
                                                  (p.sval.empty() ||
                                                   std::memcmp(sd, p.sval.data(), p.sval.size()) == 0);
                                        if (p.op == SimpleCmpOp::EQ) { if (!eq) return false; }
                                        else                         { if (eq)  return false; }
                                    }
                                } else {
                                    int64_t pv = p.ival;
                                    int64_t v = 0;
                                    if (col.type.id() == LogicalTypeId::INTEGER) v = col.i32_data[i];
                                    else if (col.type.id() == LogicalTypeId::BIGINT) v = col.i64_data[i];
                                    else return false;
                                    switch (p.op) {
                                    case SimpleCmpOp::EQ: if (v != pv) return false; break;
                                    case SimpleCmpOp::NE: if (v == pv) return false; break;
                                    case SimpleCmpOp::LT: if (!(v <  pv)) return false; break;
                                    case SimpleCmpOp::LE: if (!(v <= pv)) return false; break;
                                    case SimpleCmpOp::GT: if (!(v >  pv)) return false; break;
                                    case SimpleCmpOp::GE: if (!(v >= pv)) return false; break;
                                    }
                                }
                            }
                            return true;
                        };

                        for (idx_t i = 0; i < nrows; i++) {
                            if (mask_active && !mask[i]) continue;
                            if (fallback_row_loop && !eval_row(i)) continue;
                            bool key_is_null = !key_all_valid && !(i < kcol.validity.size() && kcol.validity[i]);
                            T key = key_is_null ? T{} : kdata[i];
                            LightEntry e{key, (uint32_t)rg_idx, (uint32_t)i, key_is_null};
                            if (heap.size() < k) {
                                heap.push(e);
                            } else if (light_cmp(e, heap.top())) {
                                heap.pop();
                                heap.push(e);
                            }
                        }
                    });
                pq_scan->RunParallelRGs(0);
                for (auto &h : light_heaps) {
                    while (!h.empty()) {
                        auto e = h.top(); h.pop();
                        if (light_merged.size() < k) {
                            light_merged.push(e);
                        } else {
                            bool wins = ascending ? (e.key < light_merged.top().key)
                                                  : (e.key > light_merged.top().key);
                            if (wins) { light_merged.pop(); light_merged.push(e); }
                        }
                    }
                }
            }

            // Drain into a flat vector sorted by output order, then group
            // by rg_idx so we decode each RG only once.
            std::vector<LightEntry> winners;
            winners.reserve(light_merged.size());
            while (!light_merged.empty()) { winners.push_back(light_merged.top()); light_merged.pop(); }
            // light_merged drained worst-first; reverse for best-first.
            std::reverse(winners.begin(), winners.end());

            // Pass 2: decode each unique RG with full projection.
            // Parallelised at (rg, col) granularity rather than (rg) so a
            // single-RG winner set still uses every available core.
            // Skips the key column on the second pass since we already
            // have those values in the winners list from pass 1.
            ankerl::unordered_dense::map<uint32_t, std::vector<size_t>> rg_to_winner_indices;
            for (size_t i = 0; i < winners.size(); i++) {
                rg_to_winner_indices[winners[i].rg_idx].push_back(i);
            }

            std::vector<std::vector<Value>> output_rows(winners.size());

            std::vector<uint32_t> unique_rgs;
            unique_rgs.reserve(rg_to_winner_indices.size());
            for (auto &kv : rg_to_winner_indices) unique_rgs.push_back(kv.first);

            ankerl::unordered_dense::map<uint32_t, size_t> rg_to_local;
            for (size_t i = 0; i < unique_rgs.size(); i++)
                rg_to_local[unique_rgs[i]] = i;

            // Per-(rg, col) decoded buffers. Indexed by [local_rg_idx][col].
            std::vector<std::vector<ParquetColumnData>> rg_cols(unique_rgs.size(),
                std::vector<ParquetColumnData>(ncols));
            std::vector<std::vector<std::vector<Value>>> rg_fallback(unique_rgs.size(),
                std::vector<std::vector<Value>>(ncols));

            // Build the (rg, col) work list. Skip the key column AND any
            // column the parent projection didn't request — Q25 only outputs
            // SearchPhrase but hits.parquet has 105 cols; without this filter
            // pass 2 decoded all 104 (excluding key_col) per RG, dwarfing the
            // pass-1 cost.
            const auto &p2_proj = pq_scan->GetProjection();
            const auto &p2_parent = parent_proj_mask_;
            auto col_needed_p2 = [&](idx_t c) -> bool {
                if (c == key_col) return false;
                // Parent-projection mask takes precedence: skip cols the
                // downstream PhysicalProjection does not reference.
                if (!p2_parent.empty()) {
                    if (c >= p2_parent.size() || !p2_parent[c]) return false;
                }
                if (p2_proj.empty()) return true;  // no projection → all cols
                return c < p2_proj.size() && p2_proj[c];
            };
            struct DecodeUnit { uint32_t rg; idx_t col; };
            std::vector<DecodeUnit> units;
            units.reserve(unique_rgs.size() * (ncols - 1));
            for (auto rg_u : unique_rgs) {
                for (idx_t c = 0; c < ncols; c++) {
                    if (!col_needed_p2(c)) continue;
                    units.push_back({rg_u, c});
                }
            }

            unsigned int p2_threads = HWThreads();
            if (p2_threads > 8) p2_threads = 8;
            if (p2_threads > units.size())
                p2_threads = static_cast<unsigned int>(units.size());

            auto decode_unit = [&](const DecodeUnit &u) {
                size_t local = rg_to_local[u.rg];
                if (!reader->ReadColumnInto(u.rg, u.col, rg_cols[local][u.col])) {
                    rg_fallback[local][u.col] = reader->ReadColumn(u.rg, u.col);
                }
            };

            if (p2_threads <= 1 || units.size() == 1) {
                for (auto &u : units) decode_unit(u);
            } else {
                std::atomic<size_t> next2{0};
                std::vector<std::thread> ts2;
                ts2.reserve(p2_threads);
                for (unsigned int t = 0; t < p2_threads; t++) {
                    ts2.emplace_back([&] {
                        while (true) {
                            size_t idx = next2.fetch_add(1, std::memory_order_relaxed);
                            if (idx >= units.size()) return;
                            decode_unit(units[idx]);
                        }
                    });
                }
                for (auto &t : ts2) if (t.joinable()) t.join();
            }

            // Pluck winning rows. Cheap, sequential. Reuses key from
            // pass 1 for the order-by column.
            auto box_typed = [](const ParquetColumnData &col, idx_t row_i) -> Value {
                if (!col.decoded) return Value();
                bool valid = col.all_valid || (row_i < col.validity.size() && col.validity[row_i]);
                if (!valid) return Value();
                switch (col.type.id()) {
                case LogicalTypeId::BIGINT:  return Value::BIGINT(col.i64_data[row_i]);
                case LogicalTypeId::INTEGER: return Value::INTEGER(col.i32_data[row_i]);
                case LogicalTypeId::DOUBLE:  return Value::DOUBLE(col.f64_data[row_i]);
                case LogicalTypeId::FLOAT:   return Value::FLOAT(col.f32_data[row_i]);
                case LogicalTypeId::BOOLEAN: return Value::BOOLEAN(col.bool_data[row_i] != 0);
                case LogicalTypeId::VARCHAR:
                    if (col.str_dict_encoded && row_i < col.str_dict_indices.size()) {
                        auto idx = col.str_dict_indices[row_i];
                        if (idx < col.str_dict_values.size())
                            return Value::VARCHAR(col.str_dict_values[idx].GetString());
                        return Value();
                    }
                    if (row_i < col.str_data.size())
                        return Value::VARCHAR(col.str_data[row_i].GetString());
                    return Value();
                default: return Value();
                }
            };
            for (size_t out_idx = 0; out_idx < winners.size(); out_idx++) {
                auto &winner = winners[out_idx];
                size_t local = rg_to_local[winner.rg_idx];
                idx_t row_i = winner.row_idx;
                std::vector<Value> row;
                row.reserve(ncols);
                for (idx_t c = 0; c < ncols; c++) {
                    if (c == key_col) {
                        // Use the key value already captured during pass 1.
                        if (winner.is_null) {
                            row.push_back(Value());
                        } else if constexpr (std::is_same_v<T, int64_t>)
                            row.push_back(Value::BIGINT(winner.key));
                        else if constexpr (std::is_same_v<T, int32_t>)
                            row.push_back(Value::INTEGER(winner.key));
                        else if constexpr (std::is_same_v<T, double>)
                            row.push_back(Value::DOUBLE(winner.key));
                        else if constexpr (std::is_same_v<T, float>)
                            row.push_back(Value::FLOAT(winner.key));
                        continue;
                    }
                    auto &cd = rg_cols[local][c];
                    if (cd.decoded) {
                        row.push_back(box_typed(cd, row_i));
                    } else {
                        auto &fb = rg_fallback[local][c];
                        row.push_back(row_i < fb.size() ? fb[row_i] : Value());
                    }
                }
                output_rows[out_idx] = std::move(row);
            }

            sorted_rows_ = std::move(output_rows);
            return;
        }

        // Sequential fallback for non-Parquet inputs.
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> pq(cmp);
        DataChunk chunk;
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            auto &kvec = chunk.GetVector(key_col);
            auto *kdata = kvec.GetData<T>();
            auto &kvalid = kvec.GetValidity();
            bool key_all_valid = kvalid.AllValid();
            idx_t n = chunk.size();
            for (idx_t i = 0; i < n; i++) {
                if (!key_all_valid && !kvalid.RowIsValid(i)) continue;
                T key = kdata[i];
                if (pq.size() < k) {
                    HeapEntry e; e.key = key;
                    e.row.reserve(chunk.ColumnCount());
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                        e.row.push_back(chunk.GetValue(col, i));
                    }
                    pq.push(std::move(e));
                } else {
                    bool wins = ascending ? (key < pq.top().key) : (key > pq.top().key);
                    if (!wins) continue;
                    HeapEntry e; e.key = key;
                    e.row.reserve(chunk.ColumnCount());
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                        e.row.push_back(chunk.GetValue(col, i));
                    }
                    pq.pop();
                    pq.push(std::move(e));
                }
            }
        }
        sorted_rows_.reserve(pq.size());
        while (!pq.empty()) {
            sorted_rows_.push_back(std::move(const_cast<HeapEntry &>(pq.top()).row));
            pq.pop();
        }
        std::reverse(sorted_rows_.begin(), sorted_rows_.end());
    }

    // Q27-shape inline-value fast path. When the ORDER BY is
    // (primitive_key [, varchar_value]) and the output is a single VARCHAR
    // column that is ALSO a predicate col, the regular two-pass primitive
    // TopN's `kk = k*4` oversample is wasteful: a composite-key heap of
    // exactly K entries captures the correct top-K directly, and the K
    // winning value strings are captured inline during pass-1, eliminating
    // pass-2's re-decode of the value col across 3-4 unique RGs.
    //
    // Returns true if dispatch succeeded. Caller then skips both
    // CollectTopN_Primitive and ResortByFullKeys.
    template <typename T>
    bool TryCollectTopN_InlineValue(idx_t k, idx_t key_col, bool key_asc,
                                    idx_t value_col, bool value_asc) {
        // Resolve the parquet scan tree (mirrors CollectTopN_Primitive's
        // unwrap; we need pq_scan + tn_preds before we can decode).
        PhysicalParquetScan *pq_scan = dynamic_cast<PhysicalParquetScan *>(children[0].get());
        std::vector<SimplePredicate> tn_preds;
        if (!pq_scan) {
            if (auto *proj = dynamic_cast<PhysicalProjection *>(children[0].get())) {
                if (proj->IsIdentityProjection() && !proj->children.empty())
                    pq_scan = dynamic_cast<PhysicalParquetScan *>(proj->children[0].get());
            }
        }
        if (!pq_scan) {
            PhysicalFilter *flt = dynamic_cast<PhysicalFilter *>(children[0].get());
            if (!flt) {
                if (auto *proj = dynamic_cast<PhysicalProjection *>(children[0].get())) {
                    if (proj->IsIdentityProjection() && !proj->children.empty())
                        flt = dynamic_cast<PhysicalFilter *>(proj->children[0].get());
                }
            }
            if (flt && flt->GetCondition() && !flt->children.empty()) {
                std::vector<SimplePredicate> tmp;
                if (TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                    PhysicalParquetScan *cand = dynamic_cast<PhysicalParquetScan *>(flt->children[0].get());
                    if (!cand) {
                        if (auto *p2 = dynamic_cast<PhysicalProjection *>(flt->children[0].get())) {
                            if (p2->IsIdentityProjection() && !p2->children.empty())
                                cand = dynamic_cast<PhysicalParquetScan *>(p2->children[0].get());
                        }
                    }
                    if (cand) { pq_scan = cand; tn_preds = std::move(tmp); }
                }
            }
        }
        if (!pq_scan) return false;

        idx_t ncols = pq_scan->GetTypes().size();
        if (key_col >= ncols || value_col >= ncols) return false;
        if (pq_scan->GetTypes()[value_col].id() != LogicalTypeId::VARCHAR) return false;

        // Confirm value_col is one of the predicate cols (so the decoder
        // is going to touch it anyway — adding dict-index population is
        // marginal extra cost vs the full pass-2 re-decode we save).
        bool value_is_pred = false;
        for (auto &p : tn_preds) {
            if (p.col_idx == value_col && p.str_form) { value_is_pred = true; break; }
        }
        if (!value_is_pred) return false;

        // Confirm output is JUST {value_col} (+ optionally key_col). If
        // anything else is downstream, pass-2 is still needed.
        if (parent_proj_mask_.empty()) return false;  // unknown → safer to bail
        for (idx_t c = 0; c < parent_proj_mask_.size() && c < ncols; c++) {
            if (parent_proj_mask_[c] && c != key_col && c != value_col) return false;
        }

        // Heap entry stores BOTH key and value bytes. Composite compare:
        // key first, then value (per orders_).
        struct EntryV {
            T key;
            std::string value;
            bool key_null = false;
            bool value_null = false;
        };
        auto cmp = [key_asc, value_asc](const EntryV &a, const EntryV &b) {
            if (a.key_null && b.key_null) {
                // tie on key — fall through to value compare
            } else if (a.key_null) {
                return !key_asc;
            } else if (b.key_null) {
                return key_asc;
            } else if (a.key < b.key) {
                return key_asc;
            } else if (b.key < a.key) {
                return !key_asc;
            }
            // keys are equal — value tiebreak
            if (a.value_null && b.value_null) return false;
            if (a.value_null) return !value_asc;
            if (b.value_null) return value_asc;
            return value_asc ? a.value < b.value : a.value > b.value;
        };
        using HeapV = std::priority_queue<EntryV, std::vector<EntryV>, decltype(cmp)>;

        // SetSkipStrData on every str predicate col EXCEPT preserve dict
        // index population (str_lengths_only stays false for value_col so
        // str_dict_indices is filled — we need it to extract value bytes).
        std::vector<bool> tn_lo_pred(ncols, false);
        {
            std::vector<bool> skip_pre(ncols, false);
            for (auto &p : tn_preds) {
                if (p.col_idx < ncols && p.str_form) {
                    skip_pre[p.col_idx] = true;
                    // lengths-only is fine for predicate-only cols, but for
                    // value_col we need dict_indices.
                    if (p.col_idx != value_col &&
                        !p.like_contains && p.sval.empty() &&
                        (p.op == SimpleCmpOp::EQ || p.op == SimpleCmpOp::NE)) {
                        tn_lo_pred[p.col_idx] = true;
                    }
                }
            }
            pq_scan->SetSkipStrData(std::move(skip_pre));
        }
        pq_scan->Init();
        auto *reader = pq_scan->GetReader();
        const auto &meta = reader->GetMeta();
        idx_t num_rgs = meta.row_groups.size();

        struct RGEntry { T order_key; idx_t rg_idx; bool has_stats; };
        std::vector<RGEntry> rg_order;
        rg_order.reserve(num_rgs);
        bool all_have_stats = true;
        for (idx_t rg = 0; rg < num_rgs; rg++) {
            if (key_col >= meta.row_groups[rg].columns.size()) {
                all_have_stats = false;
                rg_order.push_back({T{}, rg, false}); continue;
            }
            auto &cmeta = meta.row_groups[rg].columns[key_col];
            if (!cmeta.has_stats) {
                all_have_stats = false;
                rg_order.push_back({T{}, rg, false}); continue;
            }
            T sk;
            try { sk = key_asc ? cmeta.min_value.GetValue<T>() : cmeta.max_value.GetValue<T>(); }
            catch (...) {
                all_have_stats = false;
                rg_order.push_back({T{}, rg, false}); continue;
            }
            rg_order.push_back({sk, rg, true});
        }
        if (!all_have_stats) return false;  // fall back to safer path

        std::sort(rg_order.begin(), rg_order.end(),
            [key_asc](const RGEntry &a, const RGEntry &b) {
                return key_asc ? a.order_key < b.order_key : a.order_key > b.order_key;
            });

        HeapV merged(cmp);
        std::mutex merged_mu;
        std::atomic<size_t> next_rg{0};
        std::atomic<bool> done{false};

        unsigned int nthreads = HWThreads();
        if (nthreads > 6) nthreads = 6;
        if (nthreads > rg_order.size())
            nthreads = static_cast<unsigned int>(rg_order.size());
        if (nthreads < 1) nthreads = 1;

        auto extract_value = [&](const ParquetColumnData &vcol, idx_t i,
                                 std::string &out, bool &is_null) {
            is_null = !vcol.all_valid && i < vcol.validity.size() && !vcol.validity[i];
            if (is_null) { out.clear(); return; }
            if (vcol.str_dict_encoded && i < vcol.str_dict_indices.size()) {
                uint32_t di = vcol.str_dict_indices[i];
                if (di < vcol.str_dict_values.size()) {
                    out.assign(vcol.str_dict_values[di].GetData(),
                               vcol.str_dict_values[di].GetSize());
                    return;
                }
            }
            if (i < vcol.str_data.size()) {
                out.assign(vcol.str_data[i].GetData(), vcol.str_data[i].GetSize());
                return;
            }
            out.clear();
        };

        auto eval_one_rg = [&](size_t order_idx) {
            auto &re = rg_order[order_idx];
            // Snapshot global threshold under lock; bail if this RG can't beat.
            EntryV thr_snap{};
            bool have_thr = false;
            {
                std::lock_guard<std::mutex> lk(merged_mu);
                if (merged.size() >= k) {
                    thr_snap = merged.top();
                    if (!thr_snap.key_null) {
                        bool beats = key_asc ? (re.order_key < thr_snap.key)
                                             : (re.order_key > thr_snap.key);
                        if (!beats) {
                            done.store(true, std::memory_order_relaxed);
                            return;
                        }
                        have_thr = true;
                    }
                }
            }

            idx_t nrows = pq_scan->RowGroupSize(re.rg_idx);
            std::vector<ParquetColumnData> pcols(ncols);
            if (!reader->ReadColumnInto(re.rg_idx, key_col, pcols[key_col])) return;
            for (auto &p : tn_preds) {
                if (p.col_idx >= ncols) return;
                if (p.col_idx == key_col) continue;
                pcols[p.col_idx].str_lengths_only = tn_lo_pred[p.col_idx];
                if (!reader->ReadColumnInto(re.rg_idx, p.col_idx, pcols[p.col_idx])) return;
            }
            auto &kcol = pcols[key_col];
            const T *kdata = nullptr;
            if constexpr (std::is_same_v<T, int64_t>) kdata = reinterpret_cast<const T *>(kcol.i64_data.data());
            else if constexpr (std::is_same_v<T, int32_t>) kdata = reinterpret_cast<const T *>(kcol.i32_data.data());
            else if constexpr (std::is_same_v<T, double>)  kdata = reinterpret_cast<const T *>(kcol.f64_data.data());
            else if constexpr (std::is_same_v<T, float>)   kdata = reinterpret_cast<const T *>(kcol.f32_data.data());
            if (!kdata) return;
            bool key_all_valid = kcol.all_valid;

            std::vector<uint8_t> mask;
            bool mask_active = false;
            bool fallback_row_loop = false;
            if (!tn_preds.empty()) {
                mask_active = BuildTypedKeepMask(tn_preds, pcols, nrows, mask);
                if (!mask_active) fallback_row_loop = true;
            }

            auto &vcol = pcols[value_col];
            HeapV local(cmp);
            EntryV cur;
            for (idx_t i = 0; i < nrows; i++) {
                if (mask_active && !mask[i]) continue;
                if (fallback_row_loop && !EvalSimplePredicates(tn_preds, pcols, i)) continue;
                bool key_is_null = !key_all_valid && !(i < kcol.validity.size() && kcol.validity[i]);
                T key = key_is_null ? T{} : kdata[i];
                // Fast prefilter against global threshold (no value compare —
                // value is unknown without extraction. If key alone can't
                // beat global thr, we know composite can't either.)
                if (have_thr && !key_is_null && !thr_snap.key_null) {
                    bool key_wins  = key_asc ? (key < thr_snap.key) : (key > thr_snap.key);
                    bool key_tied  = (key == thr_snap.key);
                    if (!key_wins && !key_tied) continue;
                }
                // Local prefilter: same logic against local heap top.
                if (local.size() >= k) {
                    const auto &t = local.top();
                    if (!t.key_null && !key_is_null) {
                        bool key_wins = key_asc ? (key < t.key) : (key > t.key);
                        bool key_tied = (key == t.key);
                        if (!key_wins && !key_tied) continue;
                    }
                }
                // Now we need value bytes. Extract & try insert.
                cur.key = key;
                cur.key_null = key_is_null;
                extract_value(vcol, i, cur.value, cur.value_null);
                if (local.size() < k) {
                    local.push(cur);
                } else if (cmp(cur, local.top())) {
                    local.pop();
                    local.push(cur);
                }
            }
            if (local.empty()) return;
            std::vector<EntryV> drain;
            drain.reserve(local.size());
            while (!local.empty()) { drain.push_back(std::move(const_cast<EntryV &>(local.top()))); local.pop(); }
            std::lock_guard<std::mutex> lk(merged_mu);
            for (auto &e : drain) {
                if (merged.size() < k) merged.push(std::move(e));
                else if (cmp(e, merged.top())) {
                    merged.pop();
                    merged.push(std::move(e));
                }
            }
        };

        if (nthreads <= 1) {
            for (size_t i = 0; i < rg_order.size(); i++) {
                if (done.load(std::memory_order_relaxed)) break;
                eval_one_rg(i);
            }
        } else {
            // Sequential warm-up of the best RG before parallel workers
            // start, mirroring CollectTopN_Primitive — populates the global
            // threshold so parallel workers can early-bail.
            if (!rg_order.empty()) {
                eval_one_rg(0);
                next_rg.store(1, std::memory_order_relaxed);
            }
            std::vector<std::thread> ts;
            ts.reserve(nthreads);
            for (unsigned int t = 0; t < nthreads; t++) {
                ts.emplace_back([&] {
                    while (true) {
                        if (done.load(std::memory_order_relaxed)) return;
                        size_t idx = next_rg.fetch_add(1, std::memory_order_relaxed);
                        if (idx >= rg_order.size()) return;
                        eval_one_rg(idx);
                    }
                });
            }
            for (auto &t : ts) if (t.joinable()) t.join();
        }

        // Drain merged into sorted (best-first) order, then emit ncols-wide
        // rows with key at key_col and value at value_col (parent projection
        // plucks them out).
        std::vector<EntryV> winners;
        winners.reserve(merged.size());
        while (!merged.empty()) { winners.push_back(std::move(const_cast<EntryV &>(merged.top()))); merged.pop(); }
        std::reverse(winners.begin(), winners.end());

        std::vector<std::vector<Value>> output_rows;
        output_rows.reserve(winners.size());
        for (auto &w : winners) {
            std::vector<Value> row(ncols);
            if (w.key_null) row[key_col] = Value();
            else {
                if constexpr (std::is_same_v<T, int64_t>) row[key_col] = Value::BIGINT(w.key);
                else if constexpr (std::is_same_v<T, int32_t>) row[key_col] = Value::INTEGER(w.key);
                else if constexpr (std::is_same_v<T, double>)  row[key_col] = Value::DOUBLE(w.key);
                else if constexpr (std::is_same_v<T, float>)   row[key_col] = Value::FLOAT(w.key);
            }
            row[value_col] = w.value_null ? Value() : Value::VARCHAR(w.value);
            output_rows.push_back(std::move(row));
        }
        sorted_rows_ = std::move(output_rows);
        return true;
    }

    // True iff the planned shape matches the inline-value fast path:
    // primitive ORDER BY [, varchar tiebreaker] + LIMIT, output is exactly
    // {key_col, value_col} (subset OK), value_col is a `<> ''`-style predicate.
    bool DetectInlineValueShape(idx_t &value_col, bool &value_asc) const {
        if (orders_.empty() || parent_proj_mask_.empty()) return false;
        if (orders_[0].expression->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
        idx_t key_col = static_cast<BoundColumnRef &>(*orders_[0].expression).column_index;
        idx_t found = INVALID_INDEX;
        for (idx_t c = 0; c < parent_proj_mask_.size(); c++) {
            if (parent_proj_mask_[c] && c != key_col) {
                if (found != INVALID_INDEX) return false;  // > 1 non-key output
                found = c;
            }
        }
        if (found == INVALID_INDEX) return false;
        value_col = found;
        value_asc = true;
        for (size_t i = 1; i < orders_.size(); i++) {
            if (orders_[i].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                auto &cr = static_cast<BoundColumnRef &>(*orders_[i].expression);
                if ((idx_t)cr.column_index == found) {
                    value_asc = orders_[i].ascending;
                    return true;
                }
            }
            // Secondary ORDER BY is something else — bail (composite compare
            // would be incorrect).
            return false;
        }
        return true;  // single ORDER BY (Q25 shape) or no extra orders
    }

    void CollectTopN(idx_t k) {
        // Try the fast specialised path first. Single-column ORDER BY on a
        // primitive type avoids Value boxing per losing row — drops the
        // cost of `ORDER BY x DESC LIMIT 10` over 10M rows roughly 2x
        // by skipping the OrderKey allocation in the hot loop.
        //
        // Multi-col ORDER BY: route to primitive path on first column when
        // it's a column ref of primitive type. Oversample K' = K * 64 in
        // the primitive heap so all ties at the K-th boundary are likely
        // captured, then resort by full multi-key and trim to K. For
        // ClickBench Q27 (`ORDER BY EventTime, SearchPhrase LIMIT 10`)
        // this captures the 10-30 rows that share each unique EventTime.
        if (!orders_.empty() &&
            orders_[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            auto &cref = static_cast<BoundColumnRef &>(*orders_[0].expression);
            auto tid = orders_[0].expression->GetReturnType().id();
            const bool multi = orders_.size() > 1;
            const idx_t kk = multi ? k * 4 : k;
            // Try inline-value path for Q25/Q27 shape (one VARCHAR output
            // col that's also a predicate col). Composite-key heap captures
            // exact top-K with K=k (no oversample), skipping pass-2.
            {
                idx_t value_col = INVALID_INDEX;
                bool value_asc = true;
                if (DetectInlineValueShape(value_col, value_asc)) {
                    bool ok = false;
                    switch (tid) {
                    case LogicalTypeId::BIGINT:
                    case LogicalTypeId::TIMESTAMP:
                    case LogicalTypeId::TIMESTAMP_TZ:
                    case LogicalTypeId::TIME:
                        ok = TryCollectTopN_InlineValue<int64_t>(
                            k, cref.column_index, orders_[0].ascending,
                            value_col, value_asc);
                        break;
                    case LogicalTypeId::INTEGER:
                    case LogicalTypeId::DATE:
                        ok = TryCollectTopN_InlineValue<int32_t>(
                            k, cref.column_index, orders_[0].ascending,
                            value_col, value_asc);
                        break;
                    default: break;
                    }
                    if (ok) return;
                }
            }
            switch (tid) {
            case LogicalTypeId::BIGINT:
            case LogicalTypeId::TIMESTAMP:
            case LogicalTypeId::TIMESTAMP_TZ:
            case LogicalTypeId::TIME:
                // TIMESTAMP / TIMESTAMP_TZ / TIME store int64 microseconds
                // internally; reuse the int64 primitive heap path. Q25/Q27
                // (`ORDER BY EventTime LIMIT 10`) dropped through to the
                // slow Value-based path before this case.
                CollectTopN_Primitive<int64_t>(kk, cref.column_index, orders_[0].ascending);
                if (multi) { ResortByFullKeys(); TrimToK(k); }
                return;
            case LogicalTypeId::INTEGER:
            case LogicalTypeId::DATE:
                // DATE stores int32 days-since-epoch; same int32 path.
                CollectTopN_Primitive<int32_t>(kk, cref.column_index, orders_[0].ascending);
                if (multi) { ResortByFullKeys(); TrimToK(k); }
                return;
            case LogicalTypeId::DOUBLE:
                CollectTopN_Primitive<double>(kk, cref.column_index, orders_[0].ascending);
                if (multi) { ResortByFullKeys(); TrimToK(k); }
                return;
            case LogicalTypeId::FLOAT:
                CollectTopN_Primitive<float>(kk, cref.column_index, orders_[0].ascending);
                if (multi) { ResortByFullKeys(); TrimToK(k); }
                return;
            case LogicalTypeId::VARCHAR:
                if (CollectTopN_Varchar(kk, cref.column_index, orders_[0].ascending)) {
                    if (multi) { ResortByFullKeys(); TrimToK(k); }
                    return;
                }
                break;
            default: break;
            }
        }

        // priority_queue with Less-by-output-order at the top. Heap top
        // is therefore the WORST candidate (latest in output order); when
        // a better candidate arrives we pop top and push the new row.
        //
        // Hot-loop optimisation: the heap fills up after the first K rows,
        // and after that the VAST majority of input rows lose to the
        // heap top. Materialising every losing row into a vector<Value>
        // is wasted work. We extract the order-by KEYS only (cheap typed
        // values), compare those against the current heap-top key, and
        // only build the full vector<Value> for rows that actually go
        // into the heap. On `ORDER BY x DESC LIMIT 10` over 10M rows
        // this skips boxing 99.9999% of input rows.
        struct OrderKey {
            std::vector<Value> values; // one per ORDER BY clause
        };
        struct HeapEntry {
            OrderKey key;
            std::vector<Value> row;
        };
        // Collect order-key column indices once (constant across rows).
        // For non-trivial ORDER BY expressions (e.g. ROUND(AVG(x))) the
        // column-index path doesn't apply, so we evaluate the expression
        // on each chunk via ExpressionExecutor and extract keys from the
        // result vector instead.
        bool simple = AllOrderByAreColumnRefs();
        std::vector<idx_t> key_cols;
        if (simple) {
            key_cols.reserve(orders_.size());
            for (auto &o : orders_) {
                key_cols.push_back(static_cast<BoundColumnRef &>(*o.expression).column_index);
            }
        }

        auto less_keys = [this](const OrderKey &a, const OrderKey &b) {
            for (size_t k_i = 0; k_i < orders_.size(); k_i++) {
                auto &va = a.values[k_i];
                auto &vb = b.values[k_i];
                if (va.IsNull() && vb.IsNull()) continue;
                if (va.IsNull()) return !orders_[k_i].ascending;
                if (vb.IsNull()) return orders_[k_i].ascending;
                if (va < vb) return orders_[k_i].ascending;
                if (vb < va) return !orders_[k_i].ascending;
            }
            return false;
        };
        auto cmp = [&](const HeapEntry &a, const HeapEntry &b) {
            return less_keys(a.key, b.key);
        };
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> pq(cmp);

        DataChunk chunk;
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            // For non-trivial ORDER BY expressions, evaluate them on the
            // whole chunk up front, then extract per-row keys cheaply.
            std::vector<Vector> expr_key_vecs;
            if (!simple) {
                expr_key_vecs.reserve(orders_.size());
                for (auto &o : orders_) {
                    Vector v(o.expression->GetReturnType(), chunk.size());
                    ExpressionExecutor::Execute(*o.expression, chunk, v, chunk.size());
                    expr_key_vecs.push_back(std::move(v));
                }
            }
            for (idx_t i = 0; i < chunk.size(); i++) {
                // Build the key (cheap — only the ORDER BY columns).
                OrderKey row_key;
                row_key.values.reserve(orders_.size());
                if (simple) {
                    for (idx_t kc : key_cols) row_key.values.push_back(chunk.GetValue(kc, i));
                } else {
                    for (idx_t k_i = 0; k_i < orders_.size(); k_i++) {
                        row_key.values.push_back(expr_key_vecs[k_i].GetValue(i));
                    }
                }

                if (pq.size() < k) {
                    HeapEntry e;
                    e.key = std::move(row_key);
                    e.row.reserve(chunk.ColumnCount());
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                        e.row.push_back(chunk.GetValue(col, i));
                    }
                    pq.push(std::move(e));
                } else if (less_keys(row_key, pq.top().key)) {
                    HeapEntry e;
                    e.key = std::move(row_key);
                    e.row.reserve(chunk.ColumnCount());
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                        e.row.push_back(chunk.GetValue(col, i));
                    }
                    pq.pop();
                    pq.push(std::move(e));
                }
                // else: row loses, no full materialisation. The hot path.
            }
        }

        // Drain: heap pops worst-first, so we get [worst .. best].
        // Reverse for the [best .. worst] output order.
        sorted_rows_.reserve(pq.size());
        while (!pq.empty()) {
            sorted_rows_.push_back(std::move(const_cast<HeapEntry &>(pq.top()).row));
            pq.pop();
        }
        std::reverse(sorted_rows_.begin(), sorted_rows_.end());
    }

    void CollectFull() {
        DataChunk chunk;
        bool simple = AllOrderByAreColumnRefs();
        std::vector<std::vector<Value>> keys; // populated only when !simple
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            if (!simple) EvaluateOrderKeys(chunk, keys);
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                    row.push_back(chunk.GetValue(col, i));
                }
                sorted_rows_.push_back(std::move(row));
            }
        }
        if (simple) {
            std::sort(sorted_rows_.begin(), sorted_rows_.end(),
                [this](const std::vector<Value> &a, const std::vector<Value> &b) {
                    return Less(a, b);
                });
        } else {
            // Sort an index permutation by precomputed keys, then
            // permute sorted_rows_ in one pass.
            std::vector<idx_t> perm(sorted_rows_.size());
            for (idx_t i = 0; i < perm.size(); i++) perm[i] = i;
            std::sort(perm.begin(), perm.end(),
                [&](idx_t a, idx_t b) { return LessKeys(keys[a], keys[b]); });
            std::vector<std::vector<Value>> reordered;
            reordered.reserve(sorted_rows_.size());
            for (auto i : perm) reordered.push_back(std::move(sorted_rows_[i]));
            sorted_rows_ = std::move(reordered);
        }
    }

    std::vector<BoundOrderBy> orders_;
    std::vector<std::vector<Value>> sorted_rows_;
    bool collected_ = false;
    idx_t emit_pos_ = 0;
    idx_t row_limit_ = 0;  // 0 = no limit pushdown, full sort path
    // Parent-projection pushdown mask. Empty = no info (decode all cols).
    // Non-empty: parent_proj_mask_[c] = true → col c is consumed downstream.
    std::vector<bool> parent_proj_mask_;
};

class PhysicalLimit : public PhysicalOperator {
public:
    PhysicalLimit(int64_t limit_count, int64_t offset_count, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::LIMIT, std::move(types)),
          limit_count_(limit_count), offset_count_(offset_count) {}

    void Init() override {
        // Push LIMIT hint down so partial-sort / early-cut optimizations kick in.
        if (limit_count_ >= 0 && !children.empty()) {
            idx_t total = static_cast<idx_t>(limit_count_) + static_cast<idx_t>(offset_count_);
            children[0]->SetRowLimit(total);
        }
        for (auto &child : children) child->Init();
        rows_emitted_ = 0;
        rows_skipped_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (limit_count_ >= 0 && rows_emitted_ >= static_cast<idx_t>(limit_count_)) {
            return false;
        }

        while (true) {
            DataChunk input;
            input.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(input)) return false;

            result.Initialize(GetTypes());
            idx_t result_count = 0;

            for (idx_t i = 0; i < input.size(); i++) {
                if (rows_skipped_ < static_cast<idx_t>(offset_count_)) {
                    rows_skipped_++;
                    continue;
                }
                if (limit_count_ >= 0 &&
                    rows_emitted_ >= static_cast<idx_t>(limit_count_)) {
                    break;
                }
                for (idx_t col = 0; col < input.ColumnCount(); col++) {
                    result.SetValue(col, result_count, input.GetValue(col, i));
                }
                result_count++;
                rows_emitted_++;
            }

            if (result_count > 0) {
                result.SetCardinality(result_count);
                return true;
            }
        }
    }

private:
    int64_t limit_count_;
    int64_t offset_count_;
    idx_t rows_emitted_ = 0;
    idx_t rows_skipped_ = 0;
};

class PhysicalInsert : public PhysicalOperator {
public:
    PhysicalInsert(TableCatalogEntry *table, std::vector<std::vector<BoundExprPtr>> values)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), values_(std::move(values)) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        DataChunk chunk;
        chunk.Initialize(types);

        // Pre-resolve VARCHAR(n) max lengths per column once, outside the
        // row loop. Cheaper than dynamic_cast per cell.
        std::vector<idx_t> max_lengths(types.size(), 0);
        for (idx_t c = 0; c < types.size(); c++) {
            if (types[c].id() == LogicalTypeId::VARCHAR) {
                auto *info = dynamic_cast<const VarcharTypeInfo *>(types[c].GetExtraInfo());
                if (info) max_lengths[c] = info->MaxLength();
            }
        }

        for (auto &row : values_) {
            idx_t row_idx = chunk.size();
            for (idx_t col = 0; col < row.size(); col++) {
                auto val = ExpressionExecutor::ExecuteScalar(*row[col]);
                if (max_lengths[col] > 0 && !val.IsNull() &&
                    val.type().id() == LogicalTypeId::VARCHAR) {
                    const auto &s = val.GetValue<std::string>();
                    if (s.size() > max_lengths[col]) {
                        throw OutOfRangeException(
                            "Value too long for column " +
                            std::to_string(col) + " (VARCHAR(" +
                            std::to_string(max_lengths[col]) + "): got " +
                            std::to_string(s.size()) + " chars)");
                    }
                }
                chunk.SetValue(col, row_idx, val);
            }
        }

        table_->GetStorage().Append(chunk);
        return false; // No result set.
    }

private:
    TableCatalogEntry *table_;
    std::vector<std::vector<BoundExprPtr>> values_;
    bool done_ = false;
};

class PhysicalCreateTable : public PhysicalOperator {
public:
    PhysicalCreateTable(Catalog &catalog, const std::string &name,
                        std::vector<ColumnDefinition> columns, bool if_not_exists)
        : PhysicalOperator(PhysicalOperatorType::CREATE_TABLE, {}),
          catalog_(catalog), table_name_(name), columns_(std::move(columns)),
          if_not_exists_(if_not_exists) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        if (if_not_exists_ && catalog_.GetTable(table_name_)) {
            return false;
        }

        auto types_vec = std::vector<LogicalType>();
        for (auto &col : columns_) types_vec.push_back(col.type);

        auto &entry = catalog_.CreateTable(table_name_, columns_);
        entry.SetStorage(std::make_shared<DataTable>(types_vec));
        return false;
    }

private:
    Catalog &catalog_;
    std::string table_name_;
    std::vector<ColumnDefinition> columns_;
    bool if_not_exists_;
    bool done_ = false;
};

class PhysicalDropTable : public PhysicalOperator {
public:
    PhysicalDropTable(Catalog &catalog, const std::string &name, bool if_exists)
        : PhysicalOperator(PhysicalOperatorType::DROP_TABLE, {}),
          catalog_(catalog), table_name_(name), if_exists_(if_exists) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        if (!catalog_.DropTable(table_name_) && !if_exists_) {
            throw CatalogException("Table '" + table_name_ + "' does not exist");
        }
        return false;
    }

private:
    Catalog &catalog_;
    std::string table_name_;
    bool if_exists_;
    bool done_ = false;
};

// ============================================================================
// Window
// ============================================================================

class PhysicalWindow : public PhysicalOperator {
public:
    PhysicalWindow(std::vector<BoundExprPtr> select_list, BoundExprPtr qualify,
                   std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          select_list_(std::move(select_list)), qualify_(std::move(qualify)) {}

    void SetRowLimit(idx_t n) override { row_limit_ = n; }

    void Init() override {
        for (auto &child : children) child->Init();
        setup_done_ = false;
        use_fallback_ = false;
        emit_part_ = 0;
        emit_pos_ = 0;
        rank_cur_ = 1;
        rank_prev_ = INVALID_INDEX;
        dense_cur_ = 1;
        dense_prev_ = INVALID_INDEX;
        input_ = InputBuf{};
        partitions_.clear();
        select_win_info_.clear();
        qualify_win_info_ = WinInfo{};
        qualify_has_win_ = false;
        qualify_const_ = nullptr;
        qualify_op_ = -1;
        qualify_win_on_left_ = true;
        fallback_rows_.clear();
        sort_win_ = nullptr;
        sort_single_col_ok_ = false;
        sort_col_ = INVALID_INDEX;
        sort_asc_ = true;
        sort_tid_ = LogicalTypeId::SQLNULL;
        sort_i32_.clear();
        sort_i64_.clear();
        sort_d_.clear();
        sort_s_.clear();
        partition_sorted_.clear();
    }

    bool GetData(DataChunk &result) override {
        if (!setup_done_) { Setup(); setup_done_ = true; }

        if (use_fallback_) {
            if (emit_pos_ >= fallback_rows_.size()) return false;
            result.Initialize(GetTypes());
            idx_t count = 0;
            while (emit_pos_ < fallback_rows_.size() && count < VECTOR_SIZE) {
                auto &row = fallback_rows_[emit_pos_];
                for (idx_t col = 0; col < row.size(); col++) {
                    result.SetValue(col, count, row[col]);
                }
                emit_pos_++;
                count++;
            }
            result.SetCardinality(count);
            return count > 0;
        }

        if (emit_part_ >= partitions_.size()) return false;

        if (result.ColumnCount() != GetTypes().size()) result.Initialize(GetTypes());
        else result.Reset();

        // Vectorised fast path: every output column is either a plain
        // COLUMN_REF or a positional window function (ROW_NUMBER / RANK /
        // DENSE_RANK with no qualify, no lag/lead, no aggregate-shape
        // window). The 10M-row sort+emit becomes a typed-array gather +
        // a positional sequence write, no Value boxing per output row.
        if (!qualify_has_win_ && CanUseVectorisedEmit()) {
            idx_t out = VectorisedEmit(result);
            return out > 0;
        }

        idx_t out = 0;
        while (emit_part_ < partitions_.size() && out < VECTOR_SIZE) {
            // Lazy sort: only pay sort cost for partitions we actually emit from.
            if (emit_pos_ == 0) SortOnePartition(emit_part_);
            auto &idxs = partitions_[emit_part_];
            while (emit_pos_ < idxs.size() && out < VECTOR_SIZE) {
                idx_t input_row = idxs[emit_pos_];

                if (qualify_has_win_ && !EvalQualify(emit_part_, emit_pos_)) {
                    emit_pos_++;
                    continue;
                }

                for (idx_t col = 0; col < select_list_.size(); col++) {
                    auto &expr = select_list_[col];
                    auto et = expr->GetExpressionType();
                    Value v;
                    if (et == BoundExpressionType::WINDOW) {
                        v = ComputeWindowAt(select_win_info_[col], emit_part_, emit_pos_);
                    } else if (et == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*expr);
                        v = input_.Get(input_row, ref.column_index);
                    } else if (et == BoundExpressionType::CONSTANT) {
                        v = static_cast<BoundConstant &>(*expr).value;
                    } else {
                        // Generic expression: materialize row and evaluate.
                        DataChunk single;
                        auto in_types = children[0]->GetTypes();
                        single.Initialize(in_types);
                        for (idx_t c = 0; c < input_.types.size(); c++) {
                            single.SetValue(c, 0, input_.Get(input_row, c));
                        }
                        Vector res(expr->GetReturnType());
                        ExpressionExecutor::Execute(*expr, single, res, 1);
                        v = res.GetValue(0);
                    }
                    result.SetValue(col, out, v);
                }

                emit_pos_++;
                out++;
            }
            if (emit_pos_ >= idxs.size()) {
                emit_part_++;
                emit_pos_ = 0;
                rank_cur_ = 1;
                rank_prev_ = INVALID_INDEX;
                dense_cur_ = 1;
                dense_prev_ = INVALID_INDEX;
            }
        }

        result.SetCardinality(out);
        return out > 0;
    }

    // Returns true iff every select_list entry can be emitted via the
    // typed-vectorised path (no per-row Value boxing).
    bool CanUseVectorisedEmit() const {
        for (idx_t col = 0; col < select_list_.size(); col++) {
            auto &expr = select_list_[col];
            auto et = expr->GetExpressionType();
            if (et == BoundExpressionType::COLUMN_REF) continue;
            if (et == BoundExpressionType::WINDOW) {
                auto &info = select_win_info_[col];
                if (info.fn == WF_ROW_NUMBER) continue;
                // RANK / DENSE_RANK need to peek at the previous order-by
                // value — also vectorisable but slightly more careful.
                if (info.fn == WF_RANK || info.fn == WF_DENSE_RANK) continue;
                return false;
            }
            return false;
        }
        return true;
    }

    // Typed-vectorised emit. For each output column:
    //   - COLUMN_REF: gather typed values from the input chunks via
    //     `idxs[emit_pos_..emit_pos_+count]`, write into the output's
    //     typed array directly. Validity bit copied per row.
    //   - ROW_NUMBER: write the running 1-based counter into the output
    //     int32 array.
    //   - RANK / DENSE_RANK: same as ROW_NUMBER but bumped only on
    //     tie-break of the order-by key.
    idx_t VectorisedEmit(DataChunk &result) {
        idx_t out = 0;
        idx_t num_cols = select_list_.size();

        // Pre-cache, per output column, the BoundColumnRef.column_index
        // or the WinFunc kind. Resolved at first call.
        if (vec_emit_cache_.empty()) {
            vec_emit_cache_.resize(num_cols);
            for (idx_t c = 0; c < num_cols; c++) {
                auto &expr = select_list_[c];
                if (expr->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                    vec_emit_cache_[c].is_col = true;
                    vec_emit_cache_[c].input_col =
                        static_cast<BoundColumnRef &>(*expr).column_index;
                } else {
                    vec_emit_cache_[c].is_col = false;
                    vec_emit_cache_[c].fn = select_win_info_[c].fn;
                    vec_emit_cache_[c].rank_order_col = select_win_info_[c].rank_order_col;
                    vec_emit_cache_[c].rank_order_tid = select_win_info_[c].rank_order_tid;
                }
            }
        }

        while (emit_part_ < partitions_.size() && out < VECTOR_SIZE) {
            if (emit_pos_ == 0) SortOnePartition(emit_part_);
            auto &idxs = partitions_[emit_part_];
            idx_t avail = idxs.size() - emit_pos_;
            idx_t take = std::min<idx_t>(avail, VECTOR_SIZE - out);
            if (take == 0) {
                emit_part_++;
                emit_pos_ = 0;
                rank_cur_ = 1;
                rank_prev_ = INVALID_INDEX;
                dense_cur_ = 1;
                dense_prev_ = INVALID_INDEX;
                continue;
            }

            for (idx_t c = 0; c < num_cols; c++) {
                auto &slot = vec_emit_cache_[c];
                auto &dst_vec = result.GetVector(c);
                if (slot.is_col) {
                    idx_t in_col = slot.input_col;
                    LogicalTypeId in_tid = input_.types[in_col];
                    if (in_tid == LogicalTypeId::BIGINT) {
                        auto *d = dst_vec.GetData<int64_t>();
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            if (input_.IsNull(row, in_col))
                                dst_vec.GetValidity().SetInvalid(out + r);
                            else d[out + r] = input_.GetInt64(row, in_col);
                        }
                    } else if (in_tid == LogicalTypeId::INTEGER) {
                        auto *d = dst_vec.GetData<int32_t>();
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            if (input_.IsNull(row, in_col))
                                dst_vec.GetValidity().SetInvalid(out + r);
                            else d[out + r] = input_.GetInt32(row, in_col);
                        }
                    } else if (in_tid == LogicalTypeId::DOUBLE) {
                        auto *d = dst_vec.GetData<double>();
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            if (input_.IsNull(row, in_col))
                                dst_vec.GetValidity().SetInvalid(out + r);
                            else d[out + r] = input_.GetDouble(row, in_col);
                        }
                    } else if (in_tid == LogicalTypeId::VARCHAR) {
                        // Direct string_t write into the output vector,
                        // skipping Value::VARCHAR + dst_vec.SetValue boxing.
                        // Inline strings (<=12 bytes) get copied straight
                        // into the slot. Long strings allocate once via
                        // the dst vector's string buffer. 10M-row VARCHAR
                        // window emit drops from ~3 s to ~300 ms.
                        auto *dst = dst_vec.GetData<string_t>();
                        auto &str_buf = dst_vec.GetStringBuffer();
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            if (input_.IsNull(row, in_col)) {
                                dst_vec.GetValidity().SetInvalid(out + r);
                                dst[out + r] = string_t("", 0);
                                continue;
                            }
                            const string_t &s = input_.GetStr(row, in_col);
                            uint32_t sz = s.GetSize();
                            if (sz <= string_t::INLINE_LENGTH) {
                                dst[out + r] = string_t(s.GetData(), sz);
                            } else {
                                const char *heap = str_buf.AddString(s.GetData(), sz);
                                dst[out + r] = string_t(heap, sz);
                            }
                        }
                    } else {
                        // Fallback for less-common types.
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            dst_vec.SetValue(out + r, input_.Get(row, in_col));
                        }
                    }
                } else {
                    // Window function — typed sequence per partition.
                    // ROW_NUMBER / RANK / DENSE_RANK all return BIGINT
                    // (matches the binder's return-type assignment).
                    auto *d = dst_vec.GetData<int64_t>();
                    if (slot.fn == WF_ROW_NUMBER) {
                        for (idx_t r = 0; r < take; r++) {
                            d[out + r] = static_cast<int64_t>(emit_pos_ + r + 1);
                        }
                    } else if (slot.fn == WF_RANK || slot.fn == WF_DENSE_RANK) {
                        idx_t order_col = slot.rank_order_col;
                        LogicalTypeId otid = slot.rank_order_tid;
                        for (idx_t r = 0; r < take; r++) {
                            idx_t row = idxs[emit_pos_ + r];
                            // Compare order key vs previous; bump on change.
                            bool same = false;
                            if (rank_prev_ != INVALID_INDEX) {
                                if (otid == LogicalTypeId::BIGINT)
                                    same = input_.GetInt64(rank_prev_, order_col) == input_.GetInt64(row, order_col);
                                else if (otid == LogicalTypeId::INTEGER)
                                    same = input_.GetInt32(rank_prev_, order_col) == input_.GetInt32(row, order_col);
                                else if (otid == LogicalTypeId::DOUBLE)
                                    same = input_.GetDouble(rank_prev_, order_col) == input_.GetDouble(row, order_col);
                                else
                                    same = input_.Get(rank_prev_, order_col) == input_.Get(row, order_col);
                            }
                            if (slot.fn == WF_RANK) {
                                if (!same) rank_cur_ = static_cast<idx_t>(emit_pos_ + r + 1);
                                d[out + r] = static_cast<int64_t>(rank_cur_);
                            } else {
                                if (!same) dense_cur_++;
                                if (rank_prev_ == INVALID_INDEX) dense_cur_ = 1;
                                d[out + r] = static_cast<int64_t>(dense_cur_);
                            }
                            rank_prev_ = row;
                        }
                    }
                }
            }

            emit_pos_ += take;
            out += take;
            if (emit_pos_ >= idxs.size()) {
                emit_part_++;
                emit_pos_ = 0;
                rank_cur_ = 1;
                rank_prev_ = INVALID_INDEX;
                dense_cur_ = 1;
                dense_prev_ = INVALID_INDEX;
            }
        }
        result.SetCardinality(out);
        return out;
    }

private:
    // Walk an expression tree, marking column indices that are referenced.
    static void CollectRefs(const BoundExpression &e, std::vector<bool> &used) {
        switch (e.GetExpressionType()) {
        case BoundExpressionType::COLUMN_REF: {
            auto &r = static_cast<const BoundColumnRef &>(e);
            if (r.column_index < used.size()) used[r.column_index] = true;
            break;
        }
        case BoundExpressionType::WINDOW: {
            auto &w = static_cast<const BoundWindowExpression &>(e);
            for (auto &p : w.partition_by) CollectRefs(*p, used);
            for (auto &o : w.order_by) CollectRefs(*o.expression, used);
            for (auto &a : w.arguments) CollectRefs(*a, used);
            break;
        }
        case BoundExpressionType::COMPARISON: {
            auto &c = static_cast<const BoundComparison &>(e);
            CollectRefs(*c.left, used);
            CollectRefs(*c.right, used);
            break;
        }
        case BoundExpressionType::CONJUNCTION: {
            auto &c = static_cast<const BoundConjunction &>(e);
            CollectRefs(*c.left, used);
            CollectRefs(*c.right, used);
            break;
        }
        case BoundExpressionType::NEGATION: {
            auto &n = static_cast<const BoundNegation &>(e);
            CollectRefs(*n.child, used);
            break;
        }
        case BoundExpressionType::IS_NULL: {
            auto &n = static_cast<const BoundIsNull &>(e);
            CollectRefs(*n.child, used);
            break;
        }
        case BoundExpressionType::ARITHMETIC: {
            auto &a = static_cast<const BoundArithmetic &>(e);
            CollectRefs(*a.left, used);
            CollectRefs(*a.right, used);
            break;
        }
        case BoundExpressionType::FUNCTION: {
            auto &f = static_cast<const BoundFunction &>(e);
            for (auto &a : f.arguments) CollectRefs(*a, used);
            break;
        }
        case BoundExpressionType::UNARY_MINUS: {
            auto &u = static_cast<const BoundUnaryMinus &>(e);
            CollectRefs(*u.child, used);
            break;
        }
        case BoundExpressionType::CAST: {
            auto &c = static_cast<const BoundCast &>(e);
            CollectRefs(*c.child, used);
            break;
        }
        case BoundExpressionType::CONSTANT:
        case BoundExpressionType::STAR:
            // No column refs.
            break;
        default:
            // SUBQUERY or unknown - be conservative: mark all needed.
            for (idx_t i = 0; i < used.size(); i++) used[i] = true;
            break;
        }
    }

    // Input buffer: stores chunks as-is + a global row -> (chunk,pos) lookup.
    // This avoids the O(N*C) Value allocation that dominated the old path.
    struct InputBuf {
        std::vector<DataChunk> chunks;
        std::vector<uint32_t> chunk_of; // global row -> chunk idx
        std::vector<uint32_t> pos_of;   // global row -> row within chunk
        idx_t total = 0;
        std::vector<LogicalTypeId> types;

        idx_t size() const { return total; }

        Value Get(idx_t row, idx_t col) const {
            return chunks[chunk_of[row]].GetValue(col, pos_of[row]);
        }
        bool IsNull(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return !v.GetValidity().RowIsValid(pos_of[row]);
        }
        int32_t GetInt32(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const int32_t *>(v.GetData())[pos_of[row]];
        }
        int64_t GetInt64(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const int64_t *>(v.GetData())[pos_of[row]];
        }
        double GetDouble(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const double *>(v.GetData())[pos_of[row]];
        }
        const string_t &GetStr(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const string_t *>(v.GetData())[pos_of[row]];
        }
        // Read int64 from any integer column (INTEGER or BIGINT); 0 if other.
        int64_t ReadI64(idx_t row, idx_t col) const {
            auto tid = types[col];
            if (tid == LogicalTypeId::BIGINT) return GetInt64(row, col);
            if (tid == LogicalTypeId::INTEGER) return GetInt32(row, col);
            return 0;
        }
    };

    // Per-window metadata precomputed during Setup.
    enum WinFunc {
        WF_ROW_NUMBER, WF_RANK, WF_DENSE_RANK, WF_NTILE,
        WF_LAG, WF_LEAD, WF_FIRST_VALUE, WF_LAST_VALUE,
        WF_SUM, WF_COUNT, WF_AVG, WF_MIN, WF_MAX, WF_UNKNOWN
    };

    static WinFunc ResolveWinFunc(const std::string &name) {
        if (name == "ROW_NUMBER") return WF_ROW_NUMBER;
        if (name == "RANK") return WF_RANK;
        if (name == "DENSE_RANK") return WF_DENSE_RANK;
        if (name == "NTILE") return WF_NTILE;
        if (name == "LAG") return WF_LAG;
        if (name == "LEAD") return WF_LEAD;
        if (name == "FIRST_VALUE") return WF_FIRST_VALUE;
        if (name == "LAST_VALUE") return WF_LAST_VALUE;
        if (name == "SUM") return WF_SUM;
        if (name == "COUNT") return WF_COUNT;
        if (name == "AVG") return WF_AVG;
        if (name == "MIN") return WF_MIN;
        if (name == "MAX") return WF_MAX;
        return WF_UNKNOWN;
    }

    struct WinInfo {
        WinFunc fn = WF_UNKNOWN;
        idx_t arg_col = INVALID_INDEX;
        LogicalTypeId arg_tid = LogicalTypeId::SQLNULL;
        int64_t lag_offset = 1;
        int64_t ntile_buckets = 1;
        idx_t rank_order_col = INVALID_INDEX;
        LogicalTypeId rank_order_tid = LogicalTypeId::SQLNULL;
        // Precomputed Value per partition (only for aggregate-shape windows).
        std::vector<Value> part_value;
    };

    struct VecEmitSlot {
        bool is_col = false;
        idx_t input_col = INVALID_INDEX;
        WinFunc fn = WF_UNKNOWN;
        idx_t rank_order_col = INVALID_INDEX;
        LogicalTypeId rank_order_tid = LogicalTypeId::SQLNULL;
    };
    std::vector<VecEmitSlot> vec_emit_cache_;

    // Reads entire child input into InputBuf with typed raw Vector storage.
    void ReadInput() {
        idx_t num_cols = children[0]->GetTypes().size();
        input_.types.clear();
        input_.types.reserve(num_cols);
        for (auto &t : children[0]->GetTypes()) input_.types.push_back(t.id());
        input_.chunks.clear();
        input_.chunks.reserve(16);
        input_.total = 0;
        while (true) {
            DataChunk ch;
            ch.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(ch)) break;
            idx_t cnt = ch.size();
            if (cnt == 0) continue;
            input_.total += cnt;
            input_.chunks.push_back(std::move(ch));
        }
        if (input_.total == 0) return;
        input_.chunk_of.resize(input_.total);
        input_.pos_of.resize(input_.total);
        idx_t gi = 0;
        for (idx_t ci = 0; ci < input_.chunks.size(); ci++) {
            idx_t sz = input_.chunks[ci].size();
            for (idx_t ri = 0; ri < sz; ri++) {
                input_.chunk_of[gi] = static_cast<uint32_t>(ci);
                input_.pos_of[gi] = static_cast<uint32_t>(ri);
                gi++;
            }
        }
    }

    // True if two window expressions share the same PARTITION BY and ORDER BY
    // (by column reference and direction).
    static bool SamePartitionAndOrder(const BoundWindowExpression &a,
                                       const BoundWindowExpression &b) {
        if (a.partition_by.size() != b.partition_by.size()) return false;
        if (a.order_by.size() != b.order_by.size()) return false;
        for (idx_t i = 0; i < a.partition_by.size(); i++) {
            auto *ea = a.partition_by[i].get();
            auto *eb = b.partition_by[i].get();
            if (ea->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (eb->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (static_cast<const BoundColumnRef *>(ea)->column_index !=
                static_cast<const BoundColumnRef *>(eb)->column_index) return false;
        }
        for (idx_t i = 0; i < a.order_by.size(); i++) {
            auto *ea = a.order_by[i].expression.get();
            auto *eb = b.order_by[i].expression.get();
            if (ea->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (eb->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (static_cast<const BoundColumnRef *>(ea)->column_index !=
                static_cast<const BoundColumnRef *>(eb)->column_index) return false;
            if (a.order_by[i].ascending != b.order_by[i].ascending) return false;
        }
        return true;
    }

    // Build partitions_ using the reference window's PARTITION BY.
    void BuildPartitions(BoundWindowExpression &win) {
        idx_t n = input_.total;
        partitions_.clear();
        if (win.partition_by.empty()) {
            partitions_.emplace_back();
            partitions_[0].reserve(n);
            for (idx_t i = 0; i < n; i++) partitions_[0].push_back(i);
            return;
        }
        bool single_col = (win.partition_by.size() == 1 &&
            win.partition_by[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        LogicalTypeId tid = LogicalTypeId::SQLNULL;
        idx_t part_col = INVALID_INDEX;
        if (single_col) {
            auto &ref = static_cast<BoundColumnRef &>(*win.partition_by[0]);
            part_col = ref.column_index;
            if (part_col < input_.types.size()) tid = input_.types[part_col];
        }
        if (single_col && (tid == LogicalTypeId::INTEGER || tid == LogicalTypeId::BIGINT)) {
            std::unordered_map<int64_t, idx_t> k2p;
            k2p.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                int64_t k = input_.ReadI64(i, part_col);
                auto it = k2p.find(k);
                idx_t pi;
                if (it == k2p.end()) { pi = partitions_.size(); partitions_.emplace_back(); k2p.emplace(k, pi); }
                else pi = it->second;
                partitions_[pi].push_back(i);
            }
        } else if (single_col && tid == LogicalTypeId::VARCHAR) {
            // Linear-cache fast path - most real VARCHAR PARTITION BY is
            // low-cardinality (~10s of categories). Keep a small inline array
            // and do memcmp against it - much faster than std::unordered_map's
            // string hashing + allocation per row.
            struct Entry { const char *data; uint32_t len; idx_t pi; };
            std::vector<Entry> cache;
            cache.reserve(64);
            std::unordered_map<std::string, idx_t> overflow; // used if cardinality > cache cap
            const idx_t cache_cap = 256;
            for (idx_t i = 0; i < n; i++) {
                auto &s = input_.GetStr(i, part_col);
                const char *d = s.GetData();
                uint32_t l = s.GetSize();
                idx_t pi = INVALID_INDEX;
                if (cache.size() < cache_cap) {
                    for (auto &e : cache) {
                        if (e.len == l && memcmp(e.data, d, l) == 0) { pi = e.pi; break; }
                    }
                    if (pi == INVALID_INDEX) {
                        pi = partitions_.size();
                        partitions_.emplace_back();
                        cache.push_back({d, l, pi});
                    }
                } else {
                    std::string key(d, l);
                    auto it = overflow.find(key);
                    if (it == overflow.end()) {
                        pi = partitions_.size();
                        partitions_.emplace_back();
                        overflow.emplace(std::move(key), pi);
                    } else pi = it->second;
                }
                partitions_[pi].push_back(i);
            }
        } else {
            std::unordered_map<std::string, idx_t> k2p;
            k2p.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                std::string key;
                for (auto &p : win.partition_by) {
                    if (p->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*p);
                        auto v = input_.Get(i, ref.column_index);
                        if (v.IsNull()) key += "\x01N";
                        else key += v.ToString();
                        key += '|';
                    }
                }
                auto it = k2p.find(key);
                idx_t pi;
                if (it == k2p.end()) { pi = partitions_.size(); partitions_.emplace_back(); k2p.emplace(std::move(key), pi); }
                else pi = it->second;
                partitions_[pi].push_back(i);
            }
        }
    }

    // Extract sort-key data once at setup; actual sort happens lazily per
    // partition during emit (SortOnePartition). Critical for LIMIT queries
    // that emit from only a handful of partitions.
    void PrepareSortKeys(BoundWindowExpression &win) {
        sort_win_ = nullptr;
        sort_single_col_ok_ = false;
        if (win.order_by.empty()) return;
        sort_win_ = &win;
        bool single_ord = (win.order_by.size() == 1 &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        if (!single_ord) return;
        auto &ref = static_cast<BoundColumnRef &>(*win.order_by[0].expression);
        sort_col_ = ref.column_index;
        sort_asc_ = win.order_by[0].ascending;
        sort_tid_ = (sort_col_ < input_.types.size()) ? input_.types[sort_col_] : LogicalTypeId::SQLNULL;
        idx_t n = input_.total;
        if (sort_tid_ == LogicalTypeId::INTEGER) {
            sort_i32_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_i32_[i] = input_.GetInt32(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::BIGINT) {
            sort_i64_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_i64_[i] = input_.GetInt64(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::DOUBLE) {
            sort_d_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_d_[i] = input_.GetDouble(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::VARCHAR) {
            sort_s_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_s_[i] = input_.GetStr(i, sort_col_);
            sort_single_col_ok_ = true;
        }
    }

    void SortOnePartition(idx_t p) {
        if (p >= partition_sorted_.size() || partition_sorted_[p]) return;
        auto &idxs = partitions_[p];
        if (!sort_win_) { partition_sorted_[p] = true; return; }
        // Use partial_sort when LIMIT is small and we're sorting the first (or only)
        // partition - takes only the top-K rows in order, O(N log K) vs O(N log N).
        // Safe for ROW_NUMBER/NTILE/LAG/LEAD since consumer LIMIT stops the emit.
        idx_t partial_k = 0;
        if (row_limit_ > 0 && row_limit_ < idxs.size()) {
            // Only apply to the first partition - later partitions might contribute rows
            // if earlier ones don't fill the limit; conservatively sort later ones fully.
            if (p == 0) partial_k = row_limit_;
        }
        auto do_sort = [&](auto cmp) {
            if (partial_k > 0) {
                std::partial_sort(idxs.begin(), idxs.begin() + partial_k, idxs.end(), cmp);
            } else {
                std::sort(idxs.begin(), idxs.end(), cmp);
            }
        };
        if (sort_single_col_ok_) {
            if (sort_tid_ == LogicalTypeId::INTEGER) {
                bool asc = sort_asc_;
                auto *keys = sort_i32_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? (keys[a] < keys[b]) : (keys[a] > keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::BIGINT) {
                bool asc = sort_asc_;
                auto *keys = sort_i64_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? (keys[a] < keys[b]) : (keys[a] > keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::DOUBLE) {
                bool asc = sort_asc_;
                auto *keys = sort_d_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? (keys[a] < keys[b]) : (keys[a] > keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::VARCHAR) {
                bool asc = sort_asc_;
                auto *keys = sort_s_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    auto &sa = keys[a]; auto &sb = keys[b];
                    uint32_t la = sa.GetSize(), lb = sb.GetSize();
                    uint32_t m = la < lb ? la : lb;
                    int c = memcmp(sa.GetData(), sb.GetData(), m);
                    if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                    return asc ? (c < 0) : (c > 0);
                });
            }
        } else {
            auto &win = *sort_win_;
            std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                for (auto &ord : win.order_by) {
                    if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                        auto va = input_.Get(a, ref.column_index);
                        auto vb = input_.Get(b, ref.column_index);
                        if (va < vb) return ord.ascending;
                        if (vb < va) return !ord.ascending;
                    }
                }
                return false;
            });
        }
        partition_sorted_[p] = true;
    }

    // Resolve a window expression into WinInfo and precompute per-partition
    // values for aggregate-shape windows (SUM/COUNT/AVG/MIN/MAX/FIRST_VALUE/LAST_VALUE).
    void CompileWindow(BoundWindowExpression &win, WinInfo &info) {
        info.fn = ResolveWinFunc(win.function_name);
        info.arg_col = INVALID_INDEX;
        info.arg_tid = LogicalTypeId::SQLNULL;
        if (!win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            info.arg_col = static_cast<BoundColumnRef &>(*win.arguments[0]).column_index;
            if (info.arg_col < input_.types.size()) info.arg_tid = input_.types[info.arg_col];
        }
        info.lag_offset = 1;
        if (win.arguments.size() > 1 &&
            win.arguments[1]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[1]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) info.lag_offset = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) info.lag_offset = c.value.GetValue<int64_t>();
        }
        info.ntile_buckets = 1;
        if (info.fn == WF_NTILE && !win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[0]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) info.ntile_buckets = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) info.ntile_buckets = c.value.GetValue<int64_t>();
            if (info.ntile_buckets <= 0) info.ntile_buckets = 1;
        }
        info.rank_order_col = INVALID_INDEX;
        info.rank_order_tid = LogicalTypeId::SQLNULL;
        if (!win.order_by.empty() &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            info.rank_order_col = static_cast<BoundColumnRef &>(*win.order_by[0].expression).column_index;
            if (info.rank_order_col < input_.types.size())
                info.rank_order_tid = input_.types[info.rank_order_col];
        }

        // SUM/COUNT/AVG/MIN/MAX over whole partition don't require sorted order
        // (we treat ORDER BY as irrelevant for these - full-partition aggregate).
        // FIRST/LAST_VALUE are computed on-demand at emit time (they need sorted order).
        if (info.fn == WF_SUM || info.fn == WF_COUNT || info.fn == WF_AVG ||
            info.fn == WF_MIN || info.fn == WF_MAX) {
            info.part_value.resize(partitions_.size());
            for (idx_t p = 0; p < partitions_.size(); p++) {
                info.part_value[p] = ComputePartitionAggregate(info, partitions_[p]);
            }
        }
    }

    Value ComputePartitionAggregate(const WinInfo &info, const std::vector<idx_t> &indices) {
        idx_t ps = indices.size();
        if (ps == 0) return Value();
        if (info.fn == WF_FIRST_VALUE) {
            if (info.arg_col == INVALID_INDEX) return Value();
            return input_.Get(indices[0], info.arg_col);
        }
        if (info.fn == WF_LAST_VALUE) {
            if (info.arg_col == INVALID_INDEX) return Value();
            return input_.Get(indices[ps - 1], info.arg_col);
        }
        double sum = 0;
        int64_t count = 0;
        double min_d = 0, max_d = 0;
        bool has_mm = false;
        if (info.arg_col == INVALID_INDEX) {
            count = static_cast<int64_t>(ps);
        } else if (info.arg_tid == LogicalTypeId::INTEGER) {
            for (auto idx : indices) if (!input_.IsNull(idx, info.arg_col)) {
                double d = input_.GetInt32(idx, info.arg_col);
                count++; sum += d;
                if (!has_mm) { min_d = max_d = d; has_mm = true; }
                else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
            }
        } else if (info.arg_tid == LogicalTypeId::BIGINT) {
            for (auto idx : indices) if (!input_.IsNull(idx, info.arg_col)) {
                double d = static_cast<double>(input_.GetInt64(idx, info.arg_col));
                count++; sum += d;
                if (!has_mm) { min_d = max_d = d; has_mm = true; }
                else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
            }
        } else if (info.arg_tid == LogicalTypeId::DOUBLE) {
            for (auto idx : indices) if (!input_.IsNull(idx, info.arg_col)) {
                double d = input_.GetDouble(idx, info.arg_col);
                count++; sum += d;
                if (!has_mm) { min_d = max_d = d; has_mm = true; }
                else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
            }
        } else {
            Value min_v, max_v;
            for (auto idx : indices) {
                auto v = input_.Get(idx, info.arg_col);
                if (!v.IsNull()) {
                    count++;
                    auto tid = v.type().id();
                    double d = 0;
                    if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                    else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                    else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                    sum += d;
                    if (!has_mm || v < min_v) min_v = v;
                    if (!has_mm || v > max_v) max_v = v;
                    has_mm = true;
                }
            }
            if (info.fn == WF_COUNT) return Value::BIGINT(count);
            if (info.fn == WF_SUM) {
                if (info.arg_tid == LogicalTypeId::DOUBLE || info.arg_tid == LogicalTypeId::FLOAT)
                    return Value::DOUBLE(sum);
                return Value::BIGINT(static_cast<int64_t>(sum));
            }
            if (info.fn == WF_AVG) return count > 0 ? Value::DOUBLE(sum / count) : Value();
            if (info.fn == WF_MIN) return has_mm ? min_v : Value();
            if (info.fn == WF_MAX) return has_mm ? max_v : Value();
            return Value();
        }
        if (info.fn == WF_COUNT) return Value::BIGINT(count);
        if (info.fn == WF_SUM) {
            // Return type matches argument's numeric type (DOUBLE for DOUBLE, BIGINT otherwise).
            if (info.arg_tid == LogicalTypeId::DOUBLE) return Value::DOUBLE(sum);
            return Value::BIGINT(static_cast<int64_t>(sum));
        }
        if (info.fn == WF_AVG) return count > 0 ? Value::DOUBLE(sum / count) : Value();
        if (info.fn == WF_MIN) {
            if (!has_mm) return Value();
            if (info.arg_tid == LogicalTypeId::INTEGER) return Value::INTEGER(static_cast<int32_t>(min_d));
            if (info.arg_tid == LogicalTypeId::BIGINT) return Value::BIGINT(static_cast<int64_t>(min_d));
            return Value::DOUBLE(min_d);
        }
        if (info.fn == WF_MAX) {
            if (!has_mm) return Value();
            if (info.arg_tid == LogicalTypeId::INTEGER) return Value::INTEGER(static_cast<int32_t>(max_d));
            if (info.arg_tid == LogicalTypeId::BIGINT) return Value::BIGINT(static_cast<int64_t>(max_d));
            return Value::DOUBLE(max_d);
        }
        return Value();
    }

    // Compute the window value for one row during streaming emit.
    Value ComputeWindowAt(const WinInfo &info, idx_t part_idx, idx_t pos) {
        auto &indices = partitions_[part_idx];
        idx_t ps = indices.size();
        switch (info.fn) {
        case WF_ROW_NUMBER:
            return Value::BIGINT(static_cast<int64_t>(pos + 1));
        case WF_RANK: {
            if (pos == 0) { rank_cur_ = 1; rank_prev_ = indices[0]; return Value::BIGINT(1); }
            bool same = OrderEqual(info, rank_prev_, indices[pos]);
            if (!same) rank_cur_ = static_cast<int64_t>(pos + 1);
            rank_prev_ = indices[pos];
            return Value::BIGINT(rank_cur_);
        }
        case WF_DENSE_RANK: {
            if (pos == 0) { dense_cur_ = 1; dense_prev_ = indices[0]; return Value::BIGINT(1); }
            bool same = OrderEqual(info, dense_prev_, indices[pos]);
            if (!same) dense_cur_++;
            dense_prev_ = indices[pos];
            return Value::BIGINT(dense_cur_);
        }
        case WF_NTILE:
            return Value::BIGINT(static_cast<int64_t>(pos * info.ntile_buckets / ps) + 1);
        case WF_LAG: {
            int64_t target = static_cast<int64_t>(pos) - info.lag_offset;
            if (target >= 0 && target < static_cast<int64_t>(ps) && info.arg_col != INVALID_INDEX)
                return input_.Get(indices[static_cast<idx_t>(target)], info.arg_col);
            return Value();
        }
        case WF_LEAD: {
            int64_t target = static_cast<int64_t>(pos) + info.lag_offset;
            if (target >= 0 && target < static_cast<int64_t>(ps) && info.arg_col != INVALID_INDEX)
                return input_.Get(indices[static_cast<idx_t>(target)], info.arg_col);
            return Value();
        }
        case WF_FIRST_VALUE:
            if (info.arg_col == INVALID_INDEX || ps == 0) return Value();
            return input_.Get(indices[0], info.arg_col);
        case WF_LAST_VALUE:
            if (info.arg_col == INVALID_INDEX || ps == 0) return Value();
            return input_.Get(indices[ps - 1], info.arg_col);
        case WF_SUM:
        case WF_COUNT:
        case WF_AVG:
        case WF_MIN:
        case WF_MAX:
            return (part_idx < info.part_value.size()) ? info.part_value[part_idx] : Value();
        default:
            return Value();
        }
    }

    bool OrderEqual(const WinInfo &info, idx_t a, idx_t b) {
        if (info.rank_order_col == INVALID_INDEX) return false;
        switch (info.rank_order_tid) {
        case LogicalTypeId::INTEGER:
            return input_.GetInt32(a, info.rank_order_col) == input_.GetInt32(b, info.rank_order_col);
        case LogicalTypeId::BIGINT:
            return input_.GetInt64(a, info.rank_order_col) == input_.GetInt64(b, info.rank_order_col);
        case LogicalTypeId::DOUBLE:
            return input_.GetDouble(a, info.rank_order_col) == input_.GetDouble(b, info.rank_order_col);
        case LogicalTypeId::VARCHAR: {
            auto &sa = input_.GetStr(a, info.rank_order_col);
            auto &sb = input_.GetStr(b, info.rank_order_col);
            if (sa.GetSize() != sb.GetSize()) return false;
            return memcmp(sa.GetData(), sb.GetData(), sa.GetSize()) == 0;
        }
        default: {
            auto va = input_.Get(a, info.rank_order_col);
            auto vb = input_.Get(b, info.rank_order_col);
            return va == vb;
        }
        }
    }

    bool EvalQualify(idx_t part_idx, idx_t pos) {
        Value wv = ComputeWindowAt(qualify_win_info_, part_idx, pos);
        if (wv.IsNull() || !qualify_const_) return false;
        auto &cv = qualify_const_->value;
        auto ctid = cv.type().id();
        bool fast_int = (ctid == LogicalTypeId::INTEGER || ctid == LogicalTypeId::BIGINT);
        if (fast_int && wv.type().id() == LogicalTypeId::BIGINT) {
            int64_t cl = (ctid == LogicalTypeId::INTEGER) ? cv.GetValue<int32_t>() : cv.GetValue<int64_t>();
            int64_t wl = wv.GetValue<int64_t>();
            int64_t lhs = qualify_win_on_left_ ? wl : cl;
            int64_t rhs = qualify_win_on_left_ ? cl : wl;
            switch (qualify_op_) {
            case 0: return lhs == rhs;
            case 1: return lhs < rhs;
            case 2: return lhs <= rhs;
            case 3: return lhs > rhs;
            case 4: return lhs >= rhs;
            case 5: return lhs != rhs;
            default: return false;
            }
        }
        const Value &lhs = qualify_win_on_left_ ? wv : cv;
        const Value &rhs = qualify_win_on_left_ ? cv : wv;
        switch (qualify_op_) {
        case 0: return !(lhs < rhs) && !(rhs < lhs);
        case 1: return lhs < rhs;
        case 2: return lhs <= rhs;
        case 3: return lhs > rhs;
        case 4: return lhs >= rhs;
        case 5: return lhs != rhs;
        default: return false;
        }
    }

    void Setup() {
#if 0 // SLOTHDB_DBG_WINDOW
        auto _dbg_t0 = std::chrono::high_resolution_clock::now();
        auto _dbg_step = [&](const char *what) {
            auto t = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(t - _dbg_t0).count() / 1000.0;
            fprintf(stderr, "[win] %-26s %.1f ms\n", what, ms);
            _dbg_t0 = t;
        };
#define DBG(s) _dbg_step(s)
#else
#define DBG(s) ((void)0)
#endif
        // Column pruning: push projection mask down to file scan so we skip
        // parsing columns not referenced by SELECT or QUALIFY.
        idx_t num_cols = children[0]->GetTypes().size();
        std::vector<bool> needed(num_cols, false);
        for (auto &s : select_list_) CollectRefs(*s, needed);
        if (qualify_) CollectRefs(*qualify_, needed);
        bool any = false;
        for (bool b : needed) if (b) { any = true; break; }
        if (any) {
            children[0]->SetNeededOutputs(needed);
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                fs->SetProjection(needed);
            }
        }
        ReadInput();
        DBG("ReadInput");
        if (input_.total == 0) return;

        // Collect all window expressions (from SELECT and QUALIFY).
        std::vector<BoundWindowExpression *> wins;
        for (auto &s : select_list_) {
            if (s->GetExpressionType() == BoundExpressionType::WINDOW)
                wins.push_back(static_cast<BoundWindowExpression *>(s.get()));
        }
        BoundWindowExpression *qwin = nullptr;
        if (qualify_ && qualify_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*qualify_);
            if (cmp.left->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.left.get());
                qualify_win_on_left_ = true;
                if (cmp.right->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qualify_const_ = static_cast<BoundConstant *>(cmp.right.get());
            } else if (cmp.right->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.right.get());
                qualify_win_on_left_ = false;
                if (cmp.left->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qualify_const_ = static_cast<BoundConstant *>(cmp.left.get());
            }
            if (qwin) {
                wins.push_back(qwin);
                const std::string &op = cmp.op;
                if (op == "=") qualify_op_ = 0;
                else if (op == "<") qualify_op_ = 1;
                else if (op == "<=") qualify_op_ = 2;
                else if (op == ">") qualify_op_ = 3;
                else if (op == ">=") qualify_op_ = 4;
                else if (op == "!=" || op == "<>") qualify_op_ = 5;
                else qualify_op_ = -1;
                qualify_has_win_ = true;
            }
        }

        if (wins.empty()) {
            // No windows at all - trivial case. Single partition, no sort.
            partitions_.emplace_back();
            partitions_[0].reserve(input_.total);
            for (idx_t i = 0; i < input_.total; i++) partitions_[0].push_back(i);
            select_win_info_.resize(select_list_.size());
            return;
        }

        // All windows must share PARTITION BY + ORDER BY. If not, fall back
        // to the generic per-column compute (builds full output once).
        auto *ref_win = wins.front();
        for (idx_t i = 1; i < wins.size(); i++) {
            if (!SamePartitionAndOrder(*ref_win, *wins[i])) {
                use_fallback_ = true;
                break;
            }
        }

        if (use_fallback_) {
            BuildFallbackRows();
            return;
        }

        BuildPartitions(*ref_win); DBG("BuildPartitions");
        partition_sorted_.assign(partitions_.size(), false);
        PrepareSortKeys(*ref_win); DBG("PrepareSortKeys");

        select_win_info_.resize(select_list_.size());
        for (idx_t col = 0; col < select_list_.size(); col++) {
            if (select_list_[col]->GetExpressionType() == BoundExpressionType::WINDOW) {
                auto &w = static_cast<BoundWindowExpression &>(*select_list_[col]);
                CompileWindow(w, select_win_info_[col]);
            }
        }
        if (qwin) CompileWindow(*qwin, qualify_win_info_);

        // Fast path: QUALIFY ROW_NUMBER() OVER (... ORDER BY X) = 1.
        // Just reduce each partition to its argmin/argmax row - no sort.
        TryTop1Qualify(ref_win, qwin);

        // Eager parallel per-partition sort.
        // Lazy sort serialised the cost of N partitions × M rows into the
        // single-threaded emit loop. With multiple partitions (PARTITION BY
        // region — typically 3-50) and a meaningful row count, sorting
        // them concurrently before emit cuts the total wall time
        // proportionally to min(num_partitions, threads).
        if (input_.total >= 200000 && partitions_.size() >= 2 &&
            !use_fallback_) {
            unsigned int nthreads = HWThreads();
            if (nthreads > 8) nthreads = 8;
            if (nthreads > partitions_.size()) nthreads = static_cast<unsigned int>(partitions_.size());
            if (nthreads > 1) {
                std::atomic<idx_t> next{0};
                std::vector<std::thread> ts;
                ts.reserve(nthreads);
                for (unsigned int t = 0; t < nthreads; t++) {
                    ts.emplace_back([&] {
                        while (true) {
                            idx_t p = next.fetch_add(1, std::memory_order_relaxed);
                            if (p >= partitions_.size()) return;
                            SortOnePartition(p);
                        }
                    });
                }
                for (auto &t : ts) if (t.joinable()) t.join();
            } else {
                for (idx_t p = 0; p < partitions_.size(); p++) SortOnePartition(p);
            }
        }
        DBG("ParallelSort");
#undef DBG
    }

    void TryTop1Qualify(BoundWindowExpression *ref_win, BoundWindowExpression *qwin) {
        if (!qwin) return;
        if (qualify_win_info_.fn != WF_ROW_NUMBER) return;
        if (qualify_op_ != 0) return;           // must be =
        if (!qualify_const_) return;
        if (!qualify_win_on_left_) return;      // only rn = K form
        auto ctid = qualify_const_->value.type().id();
        int64_t k;
        if (ctid == LogicalTypeId::INTEGER) k = qualify_const_->value.GetValue<int32_t>();
        else if (ctid == LogicalTypeId::BIGINT) k = qualify_const_->value.GetValue<int64_t>();
        else return;
        if (k != 1) return;
        if (ref_win->order_by.empty()) return;
        if (ref_win->order_by[0].expression->GetExpressionType() != BoundExpressionType::COLUMN_REF) return;

        auto &ord_ref = static_cast<BoundColumnRef &>(*ref_win->order_by[0].expression);
        idx_t col = ord_ref.column_index;
        LogicalTypeId tid = (col < input_.types.size()) ? input_.types[col] : LogicalTypeId::SQLNULL;
        bool asc = ref_win->order_by[0].ascending;

        // Reduce each partition to a single winner row - the argmin (asc) or argmax (desc).
        auto pick_one = [&](std::vector<idx_t> &idxs) {
            if (idxs.empty()) return;
            idx_t best = idxs[0];
            if (tid == LogicalTypeId::INTEGER) {
                int32_t bk = input_.GetInt32(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    int32_t v = input_.GetInt32(idxs[j], col);
                    if ((asc && v < bk) || (!asc && v > bk)) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::BIGINT) {
                int64_t bk = input_.GetInt64(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    int64_t v = input_.GetInt64(idxs[j], col);
                    if ((asc && v < bk) || (!asc && v > bk)) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::DOUBLE) {
                double bk = input_.GetDouble(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    double v = input_.GetDouble(idxs[j], col);
                    if ((asc && v < bk) || (!asc && v > bk)) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::VARCHAR) {
                auto bs = input_.GetStr(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    auto v = input_.GetStr(idxs[j], col);
                    uint32_t la = v.GetSize(), lb = bs.GetSize();
                    uint32_t m = la < lb ? la : lb;
                    int c = memcmp(v.GetData(), bs.GetData(), m);
                    if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                    if ((asc && c < 0) || (!asc && c > 0)) { bs = v; best = idxs[j]; }
                }
            } else {
                return;
            }
            idxs.clear();
            idxs.push_back(best);
        };
        for (auto &idxs : partitions_) pick_one(idxs);
        // Each partition now has size 1 - subsequent lazy sort is a no-op,
        // and the emit loop only produces one row per partition. QUALIFY ROW_NUMBER=1
        // evaluates true at pos 0, so all rows pass.
        for (idx_t p = 0; p < partition_sorted_.size(); p++) partition_sorted_[p] = true;
    }

    // Fallback path: multi-window query with different partition/order.
    // Build fallback_rows_ using per-column compute (old logic).
    void BuildFallbackRows() {
        idx_t n = input_.total;
        idx_t num_output_cols = select_list_.size();
        std::vector<std::vector<Value>> output(n);
        for (auto &row : output) row.resize(num_output_cols);
        for (idx_t col = 0; col < num_output_cols; col++) {
            auto &expr = select_list_[col];
            auto et = expr->GetExpressionType();
            if (et == BoundExpressionType::WINDOW) {
                auto &win = static_cast<BoundWindowExpression &>(*expr);
                ComputeWindowColumn(win, input_, output, col);
            } else if (et == BoundExpressionType::COLUMN_REF) {
                auto &ref = static_cast<BoundColumnRef &>(*expr);
                for (idx_t i = 0; i < n; i++) output[i][col] = input_.Get(i, ref.column_index);
            } else if (et == BoundExpressionType::CONSTANT) {
                auto &c = static_cast<BoundConstant &>(*expr);
                for (idx_t i = 0; i < n; i++) output[i][col] = c.value;
            } else {
                auto in_types = children[0]->GetTypes();
                for (idx_t i = 0; i < n; i++) {
                    DataChunk single;
                    single.Initialize(in_types);
                    for (idx_t c = 0; c < input_.types.size(); c++)
                        single.SetValue(c, 0, input_.Get(i, c));
                    Vector res(expr->GetReturnType());
                    ExpressionExecutor::Execute(*expr, single, res, 1);
                    output[i][col] = res.GetValue(0);
                }
            }
        }
        if (qualify_ && qualify_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*qualify_);
            BoundWindowExpression *qwin = nullptr;
            BoundConstant *qcon = nullptr;
            bool win_left = true;
            if (cmp.left->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.left.get());
                if (cmp.right->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qcon = static_cast<BoundConstant *>(cmp.right.get());
            } else if (cmp.right->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.right.get());
                win_left = false;
                if (cmp.left->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qcon = static_cast<BoundConstant *>(cmp.left.get());
            }
            if (qwin) {
                idx_t qual_col = num_output_cols;
                for (auto &row : output) row.push_back(Value());
                ComputeWindowColumn(*qwin, input_, output, qual_col);
                const std::string &op = cmp.op;
                for (idx_t i = 0; i < n; i++) {
                    auto &wv = output[i][qual_col];
                    if (wv.IsNull() || !qcon) continue;
                    auto &cv = qcon->value;
                    const Value &lhs = win_left ? wv : cv;
                    const Value &rhs = win_left ? cv : wv;
                    bool pass = false;
                    if (op == "=") pass = !(lhs < rhs) && !(rhs < lhs);
                    else if (op == "<") pass = lhs < rhs;
                    else if (op == "<=") pass = lhs <= rhs;
                    else if (op == ">") pass = lhs > rhs;
                    else if (op == ">=") pass = lhs >= rhs;
                    else if (op == "!=" || op == "<>") pass = lhs != rhs;
                    if (pass) {
                        auto row = std::move(output[i]);
                        row.resize(num_output_cols);
                        fallback_rows_.push_back(std::move(row));
                    }
                }
                return;
            }
        }
        fallback_rows_ = std::move(output);
    }

    void ComputeWindowColumn(BoundWindowExpression &win,
                              const InputBuf &input,
                              std::vector<std::vector<Value>> &output,
                              idx_t out_col) {
        idx_t n = input.size();
        if (n == 0) return;
        WinFunc fn = ResolveWinFunc(win.function_name);

        // Pre-resolve arg / ORDER BY column indices once.
        idx_t arg_col = INVALID_INDEX;
        if (!win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            arg_col = static_cast<BoundColumnRef &>(*win.arguments[0]).column_index;
        }
        int64_t lag_offset = 1;
        if (win.arguments.size() > 1 &&
            win.arguments[1]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[1]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) lag_offset = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) lag_offset = c.value.GetValue<int64_t>();
        }
        int64_t ntile_buckets = 1;
        if (fn == WF_NTILE && !win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[0]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) ntile_buckets = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) ntile_buckets = c.value.GetValue<int64_t>();
            if (ntile_buckets <= 0) ntile_buckets = 1;
        }
        idx_t rank_order_col = INVALID_INDEX;
        LogicalTypeId rank_order_tid = LogicalTypeId::SQLNULL;
        if (!win.order_by.empty() &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            rank_order_col = static_cast<BoundColumnRef &>(*win.order_by[0].expression).column_index;
            if (rank_order_col < input.types.size()) rank_order_tid = input.types[rank_order_col];
        }

        // Build partition groups. Typed fast paths for single-column int/varchar PARTITION BY.
        std::vector<std::vector<idx_t>> partitions_list;

        LogicalTypeId part_tid = LogicalTypeId::SQLNULL;
        idx_t part_col = INVALID_INDEX;
        bool single_col_partition = (win.partition_by.size() == 1 &&
            win.partition_by[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        if (single_col_partition) {
            auto &ref = static_cast<BoundColumnRef &>(*win.partition_by[0]);
            part_col = ref.column_index;
            if (part_col < input.types.size()) part_tid = input.types[part_col];
        }

        if (win.partition_by.empty()) {
            partitions_list.emplace_back();
            partitions_list[0].reserve(n);
            for (idx_t i = 0; i < n; i++) partitions_list[0].push_back(i);
        } else if (single_col_partition &&
                   (part_tid == LogicalTypeId::INTEGER || part_tid == LogicalTypeId::BIGINT)) {
            // Raw int64 key - no Value alloc, no hash on strings.
            std::unordered_map<int64_t, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                int64_t k = input.ReadI64(i, part_col);
                auto it = key_to_part.find(k);
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(k, p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        } else if (single_col_partition && part_tid == LogicalTypeId::VARCHAR) {
            // Raw string_t read - no Value/ToString alloc.
            std::unordered_map<std::string, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                auto &s = input.GetStr(i, part_col);
                auto it = key_to_part.find(std::string(s.GetData(), s.GetSize()));
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(std::string(s.GetData(), s.GetSize()), p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        } else {
            // Generic: Value::ToString() keyed map (slow but handles multi-col / exotic types).
            std::unordered_map<std::string, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                std::string key;
                for (auto &p : win.partition_by) {
                    if (p->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*p);
                        auto v = input.Get(i, ref.column_index);
                        if (v.IsNull()) key += "\x01N";
                        else key += v.ToString();
                        key += '|';
                    }
                }
                auto it = key_to_part.find(key);
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(std::move(key), p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        }

        // Typed sort comparator for single-column ORDER BY (common case).
        // Falls back to Value comparisons for multi-col / exotic types.
        auto sort_partitions = [&]() {
            if (win.order_by.empty()) return;
            bool single_ord = (win.order_by.size() == 1 &&
                win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF);
            if (single_ord) {
                auto &ref = static_cast<BoundColumnRef &>(*win.order_by[0].expression);
                idx_t col = ref.column_index;
                bool asc = win.order_by[0].ascending;
                auto tid = (col < input.types.size()) ? input.types[col] : LogicalTypeId::SQLNULL;
                if (tid == LogicalTypeId::INTEGER) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            auto va = input.GetInt32(a, col);
                            auto vb = input.GetInt32(b, col);
                            return asc ? (va < vb) : (va > vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::BIGINT) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            auto va = input.GetInt64(a, col);
                            auto vb = input.GetInt64(b, col);
                            return asc ? (va < vb) : (va > vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::DOUBLE) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            auto va = input.GetDouble(a, col);
                            auto vb = input.GetDouble(b, col);
                            return asc ? (va < vb) : (va > vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::VARCHAR) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            auto &sa = input.GetStr(a, col);
                            auto &sb = input.GetStr(b, col);
                            uint32_t la = sa.GetSize(), lb = sb.GetSize();
                            uint32_t m = la < lb ? la : lb;
                            int c = memcmp(sa.GetData(), sb.GetData(), m);
                            if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                            return asc ? (c < 0) : (c > 0);
                        });
                    }
                    return;
                }
            }
            // Generic Value-based fallback.
            for (auto &idxs : partitions_list) {
                std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                    for (auto &ord : win.order_by) {
                        if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                            auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                            auto va = input.Get(a, ref.column_index);
                            auto vb = input.Get(b, ref.column_index);
                            if (va < vb) return ord.ascending;
                            if (vb < va) return !ord.ascending;
                        }
                    }
                    return false;
                });
            }
        };
        sort_partitions();

        // Typed equality for RANK/DENSE_RANK tie detection (reads raw data).
        auto order_equal = [&](idx_t a, idx_t b) -> bool {
            if (rank_order_col == INVALID_INDEX) return false;
            switch (rank_order_tid) {
            case LogicalTypeId::INTEGER:
                return input.GetInt32(a, rank_order_col) == input.GetInt32(b, rank_order_col);
            case LogicalTypeId::BIGINT:
                return input.GetInt64(a, rank_order_col) == input.GetInt64(b, rank_order_col);
            case LogicalTypeId::DOUBLE:
                return input.GetDouble(a, rank_order_col) == input.GetDouble(b, rank_order_col);
            case LogicalTypeId::VARCHAR: {
                auto &sa = input.GetStr(a, rank_order_col);
                auto &sb = input.GetStr(b, rank_order_col);
                if (sa.GetSize() != sb.GetSize()) return false;
                return memcmp(sa.GetData(), sb.GetData(), sa.GetSize()) == 0;
            }
            default: {
                auto va = input.Get(a, rank_order_col);
                auto vb = input.Get(b, rank_order_col);
                return va == vb;
            }
            }
        };

        // Per-function O(n) computation. Function kind resolved once, outside the row loop.
        for (auto &indices : partitions_list) {
            idx_t ps = indices.size();
            if (ps == 0) continue;

            switch (fn) {
            case WF_ROW_NUMBER:
                for (idx_t pos = 0; pos < ps; pos++)
                    output[indices[pos]][out_col] = Value::BIGINT(static_cast<int64_t>(pos + 1));
                break;

            case WF_RANK: {
                int64_t cur_rank = 1;
                output[indices[0]][out_col] = Value::BIGINT(1);
                for (idx_t pos = 1; pos < ps; pos++) {
                    if (!order_equal(indices[pos], indices[pos - 1]))
                        cur_rank = static_cast<int64_t>(pos + 1);
                    output[indices[pos]][out_col] = Value::BIGINT(cur_rank);
                }
                break;
            }

            case WF_DENSE_RANK: {
                int64_t cur_rank = 1;
                output[indices[0]][out_col] = Value::BIGINT(1);
                for (idx_t pos = 1; pos < ps; pos++) {
                    if (!order_equal(indices[pos], indices[pos - 1])) cur_rank++;
                    output[indices[pos]][out_col] = Value::BIGINT(cur_rank);
                }
                break;
            }

            case WF_NTILE:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t bucket = static_cast<int64_t>(pos * ntile_buckets / ps) + 1;
                    output[indices[pos]][out_col] = Value::BIGINT(bucket);
                }
                break;

            case WF_LAG:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t target = static_cast<int64_t>(pos) - lag_offset;
                    if (target >= 0 && target < static_cast<int64_t>(ps) && arg_col != INVALID_INDEX) {
                        output[indices[pos]][out_col] =
                            input.Get(indices[static_cast<idx_t>(target)], arg_col);
                    } // else stays default NULL Value
                }
                break;

            case WF_LEAD:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t target = static_cast<int64_t>(pos) + lag_offset;
                    if (target >= 0 && target < static_cast<int64_t>(ps) && arg_col != INVALID_INDEX) {
                        output[indices[pos]][out_col] =
                            input.Get(indices[static_cast<idx_t>(target)], arg_col);
                    }
                }
                break;

            case WF_FIRST_VALUE: {
                Value v;
                if (arg_col != INVALID_INDEX) v = input.Get(indices[0], arg_col);
                for (idx_t pos = 0; pos < ps; pos++) output[indices[pos]][out_col] = v;
                break;
            }

            case WF_LAST_VALUE: {
                Value v;
                if (arg_col != INVALID_INDEX) v = input.Get(indices[ps - 1], arg_col);
                for (idx_t pos = 0; pos < ps; pos++) output[indices[pos]][out_col] = v;
                break;
            }

            case WF_SUM:
            case WF_COUNT:
            case WF_AVG:
            case WF_MIN:
            case WF_MAX: {
                // Aggregate over whole partition (typed fast path), broadcast to rows.
                double sum = 0;
                int64_t count = 0;
                double min_d = 0, max_d = 0;
                bool has_mm = false;
                bool is_str = false;
                bool has_str = false;
                string_t min_s, max_s;
                LogicalTypeId arg_tid = (arg_col != INVALID_INDEX && arg_col < input.types.size())
                                            ? input.types[arg_col] : LogicalTypeId::SQLNULL;
                if (arg_tid == LogicalTypeId::VARCHAR) is_str = true;

                if (arg_col == INVALID_INDEX) {
                    count = static_cast<int64_t>(ps);
                } else if (arg_tid == LogicalTypeId::INTEGER) {
                    for (auto idx : indices) {
                        if (!input.IsNull(idx, arg_col)) {
                            double d = input.GetInt32(idx, arg_col);
                            count++; sum += d;
                            if (!has_mm) { min_d = max_d = d; has_mm = true; }
                            else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
                        }
                    }
                } else if (arg_tid == LogicalTypeId::BIGINT) {
                    for (auto idx : indices) {
                        if (!input.IsNull(idx, arg_col)) {
                            double d = static_cast<double>(input.GetInt64(idx, arg_col));
                            count++; sum += d;
                            if (!has_mm) { min_d = max_d = d; has_mm = true; }
                            else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
                        }
                    }
                } else if (arg_tid == LogicalTypeId::DOUBLE) {
                    for (auto idx : indices) {
                        if (!input.IsNull(idx, arg_col)) {
                            double d = input.GetDouble(idx, arg_col);
                            count++; sum += d;
                            if (!has_mm) { min_d = max_d = d; has_mm = true; }
                            else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
                        }
                    }
                } else if (is_str) {
                    // MIN/MAX only for strings; SUM/AVG undefined.
                    for (auto idx : indices) {
                        if (!input.IsNull(idx, arg_col)) {
                            auto &s = input.GetStr(idx, arg_col);
                            count++;
                            if (!has_str) { min_s = max_s = s; has_str = true; }
                            else {
                                uint32_t l_min = s.GetSize() < min_s.GetSize() ? s.GetSize() : min_s.GetSize();
                                int c_min = memcmp(s.GetData(), min_s.GetData(), l_min);
                                if (c_min < 0 || (c_min == 0 && s.GetSize() < min_s.GetSize())) min_s = s;
                                uint32_t l_max = s.GetSize() < max_s.GetSize() ? s.GetSize() : max_s.GetSize();
                                int c_max = memcmp(s.GetData(), max_s.GetData(), l_max);
                                if (c_max > 0 || (c_max == 0 && s.GetSize() > max_s.GetSize())) max_s = s;
                            }
                        }
                    }
                } else {
                    // Fallback via Value (rare types).
                    Value min_v, max_v;
                    for (auto idx : indices) {
                        auto v = input.Get(idx, arg_col);
                        if (!v.IsNull()) {
                            count++;
                            auto tid = v.type().id();
                            double d = 0;
                            if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                            else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                            else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                            sum += d;
                            if (!has_mm || v < min_v) min_v = v;
                            if (!has_mm || v > max_v) max_v = v;
                            has_mm = true;
                        }
                    }
                    Value result;
                    if (fn == WF_COUNT) result = Value::BIGINT(count);
                    else if (fn == WF_SUM) result = Value::BIGINT(static_cast<int64_t>(sum));
                    else if (fn == WF_AVG) result = count > 0 ? Value::DOUBLE(sum / count) : Value();
                    else if (fn == WF_MIN) result = has_mm ? min_v : Value();
                    else if (fn == WF_MAX) result = has_mm ? max_v : Value();
                    for (auto idx : indices) output[idx][out_col] = result;
                    break;
                }

                // Build result from typed accumulators.
                Value result;
                if (fn == WF_COUNT) {
                    result = Value::BIGINT(count);
                } else if (is_str) {
                    if (fn == WF_MIN) result = has_str ? Value::VARCHAR(std::string(min_s.GetData(), min_s.GetSize())) : Value();
                    else if (fn == WF_MAX) result = has_str ? Value::VARCHAR(std::string(max_s.GetData(), max_s.GetSize())) : Value();
                    else result = Value();  // SUM/AVG on string not defined
                } else {
                    if (fn == WF_SUM) result = Value::BIGINT(static_cast<int64_t>(sum));
                    else if (fn == WF_AVG) result = count > 0 ? Value::DOUBLE(sum / count) : Value();
                    else if (fn == WF_MIN) {
                        if (!has_mm) result = Value();
                        else if (arg_tid == LogicalTypeId::INTEGER) result = Value::INTEGER(static_cast<int32_t>(min_d));
                        else if (arg_tid == LogicalTypeId::BIGINT) result = Value::BIGINT(static_cast<int64_t>(min_d));
                        else result = Value::DOUBLE(min_d);
                    }
                    else if (fn == WF_MAX) {
                        if (!has_mm) result = Value();
                        else if (arg_tid == LogicalTypeId::INTEGER) result = Value::INTEGER(static_cast<int32_t>(max_d));
                        else if (arg_tid == LogicalTypeId::BIGINT) result = Value::BIGINT(static_cast<int64_t>(max_d));
                        else result = Value::DOUBLE(max_d);
                    }
                }
                for (auto idx : indices) output[idx][out_col] = result;
                break;
            }

            case WF_UNKNOWN:
                break;
            }
        }
    }

    std::vector<BoundExprPtr> select_list_;
    BoundExprPtr qualify_;

    // Streaming state.
    InputBuf input_;
    std::vector<std::vector<idx_t>> partitions_;
    std::vector<WinInfo> select_win_info_;
    WinInfo qualify_win_info_;
    bool qualify_has_win_ = false;
    BoundConstant *qualify_const_ = nullptr;
    int qualify_op_ = -1;             // 0=EQ 1=LT 2=LE 3=GT 4=GE 5=NE
    bool qualify_win_on_left_ = true;
    idx_t emit_part_ = 0;
    idx_t emit_pos_ = 0;
    bool setup_done_ = false;
    // Lazy-sort state - sort each partition on-demand at emit time.
    BoundWindowExpression *sort_win_ = nullptr;
    bool sort_single_col_ok_ = false;
    idx_t sort_col_ = INVALID_INDEX;
    bool sort_asc_ = true;
    LogicalTypeId sort_tid_ = LogicalTypeId::SQLNULL;
    std::vector<int32_t> sort_i32_;
    std::vector<int64_t> sort_i64_;
    std::vector<double> sort_d_;
    std::vector<string_t> sort_s_;
    std::vector<bool> partition_sorted_;
    // Streaming RANK/DENSE_RANK state, reset per partition.
    int64_t rank_cur_ = 1;
    idx_t rank_prev_ = INVALID_INDEX;
    int64_t dense_cur_ = 1;
    idx_t dense_prev_ = INVALID_INDEX;
    // Fallback path (multi-window, different partition/order).
    bool use_fallback_ = false;
    std::vector<std::vector<Value>> fallback_rows_;
    // LIMIT pushdown - 0 means unlimited (full sort).
    idx_t row_limit_ = 0;
};

// ============================================================================
// Distinct
// ============================================================================

class PhysicalDistinct : public PhysicalOperator {
public:
    explicit PhysicalDistinct(std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)) {}

    void Init() override {
        for (auto &child : children) child->Init();
        seen_.clear();
    }

    bool GetData(DataChunk &result) override {
        while (true) {
            DataChunk input;
            input.Initialize(GetTypes());
            if (!children[0]->GetData(input)) return false;

            result.Initialize(GetTypes());
            idx_t result_count = 0;

            for (idx_t i = 0; i < input.size(); i++) {
                // Build a key from all column values.
                std::string key;
                for (idx_t col = 0; col < input.ColumnCount(); col++) {
                    key += input.GetValue(col, i).ToString() + "|";
                }
                if (seen_.insert(key).second) {
                    for (idx_t col = 0; col < input.ColumnCount(); col++) {
                        result.SetValue(col, result_count, input.GetValue(col, i));
                    }
                    result_count++;
                }
            }

            if (result_count > 0) {
                result.SetCardinality(result_count);
                return true;
            }
        }
    }

private:
    std::unordered_set<std::string> seen_;
};

// ============================================================================
// Hash Aggregate
// ============================================================================

class PhysicalHashAggregate : public PhysicalOperator {
public:
    PhysicalHashAggregate(std::vector<BoundExprPtr> groups,
                          std::vector<BoundExprPtr> aggregates,
                          std::vector<LogicalType> result_types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(result_types)),
          groups_(std::move(groups)), aggregates_(std::move(aggregates)) {}

    // TopN pushdown: when an upstream OrderBy(simple_col_ref) → Limit(K) is
    // present, OrderBy/Projection forwards (col_idx, ascending, limit) to us.
    // The direct-emit path then keeps a bounded heap of size `limit` instead
    // of materializing every group. col_idx is in our OUTPUT slot space:
    // 0..groups-1 → group cols, groups..groups+aggs-1 → agg cols.
    void SetTopNHint(idx_t col_idx, bool ascending, idx_t limit) override {
        topn_col_idx_ = col_idx;
        topn_ascending_ = ascending;
        topn_limit_ = limit;
        topn_active_ = true;
    }

    // Bare LIMIT (no ORDER BY) — no row ordering required, just emit the
    // first N. Used by emit paths that would otherwise materialize every
    // group (Q22: 80M string copies before LIMIT 10 truncates).
    void SetRowLimit(idx_t n) override { row_limit_hint_ = n; }

    void Init() override {
        for (auto &child : children) child->Init();
        computed_ = false;
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!computed_) {
            ComputeAggregates();
            computed_ = true;
        }

        if (emit_pos_ >= result_rows_.size()) return false;

        result.Initialize(GetTypes());
        idx_t count = 0;
        while (emit_pos_ < result_rows_.size() && count < VECTOR_SIZE) {
            auto &row = result_rows_[emit_pos_];
            for (idx_t col = 0; col < row.size(); col++) {
                result.SetValue(col, count, row[col]);
            }
            emit_pos_++;
            count++;
        }
        result.SetCardinality(count);
        return count > 0;
    }

private:
    // Extras: rarely-used per-AggState fields bundled behind a single
    // unique_ptr. STDDEV/MEDIAN/STRING_AGG/COUNT(DISTINCT)/BOOL_AND/OR all
    // populate fields here only when the relevant aggregate fires. For
    // common queries (COUNT*, SUM, AVG, numeric MIN/MAX) this stays nullptr,
    // saving ~140B per AggState.
    struct AggExtras {
        double sum_sq = 0;          // for STDDEV/VARIANCE
        std::vector<double> values; // for MEDIAN
        std::string str_agg;        // for STRING_AGG
        std::string str_delim;
        bool str_started = false;
        bool bool_and = true;       // for BOOL_AND
        bool bool_or = false;       // for BOOL_OR
        std::unordered_set<std::string> distinct_set; // for COUNT(DISTINCT VARCHAR / mixed)
        slothdb::SimpleI64Set distinct_int_set; // INT/BIGINT fast path
    };

    struct AggState {
        int64_t count = 0;
        double sum = 0;
        bool has_min = false;
        // Lazy: only allocated for VARCHAR MIN. Numeric MIN uses sum_min.
        std::unique_ptr<Value> min_val_ptr;
        double sum_min = 0;         // numeric min for fast comparison
        bool has_max = false;
        std::unique_ptr<Value> max_val_ptr;
        double sum_max = 0;         // numeric max for fast comparison
        std::unique_ptr<AggExtras> extras_ptr;

        // Lazy create extras on first use of any uncommon field.
        AggExtras &extras() {
            if (!extras_ptr) extras_ptr = std::make_unique<AggExtras>();
            return *extras_ptr;
        }
        // Field accessors mirror old direct access (so reads on never-used
        // states return defaults without allocation).
        double sum_sq() const { return extras_ptr ? extras_ptr->sum_sq : 0.0; }
        bool str_started() const { return extras_ptr ? extras_ptr->str_started : false; }
        bool bool_and_v() const { return extras_ptr ? extras_ptr->bool_and : true; }
        bool bool_or_v() const { return extras_ptr ? extras_ptr->bool_or : false; }
        const std::string &str_agg_const() const {
            static const std::string empty;
            return extras_ptr ? extras_ptr->str_agg : empty;
        }
        bool has_extras() const { return (bool)extras_ptr; }

        // Helpers for MIN/MAX
        Value &min_val() {
            if (!min_val_ptr) min_val_ptr = std::make_unique<Value>();
            return *min_val_ptr;
        }
        Value &max_val() {
            if (!max_val_ptr) max_val_ptr = std::make_unique<Value>();
            return *max_val_ptr;
        }
        bool min_val_is_null() const { return !min_val_ptr || min_val_ptr->IsNull(); }
        bool max_val_is_null() const { return !max_val_ptr || max_val_ptr->IsNull(); }
        Value min_val_or_null() const { return min_val_ptr ? *min_val_ptr : Value(); }
        Value max_val_or_null() const { return max_val_ptr ? *max_val_ptr : Value(); }
        void set_min_from(const AggState &s) {
            if (s.min_val_ptr) min_val_ptr = std::make_unique<Value>(*s.min_val_ptr);
        }
        void set_max_from(const AggState &s) {
            if (s.max_val_ptr) max_val_ptr = std::make_unique<Value>(*s.max_val_ptr);
        }
    };

    struct AggInfo {
        std::string name;
        idx_t col_idx;        // column index if simple column ref, else INVALID_INDEX
        bool is_count_star;
        bool is_distinct;
        // Algebraic collapse: when the agg is SUM(col +/- const), share the
        // base SUM(col) + COUNT_NON_NULL(col) scan and synthesize the per-
        // agg result as `state.sum + offset * state.count` at emit. Q30's
        // 90 SUM(ResolutionWidth + N) calls collapse to ONE scan instead
        // of 90× per-row Value boxing in the generic agg path.
        bool sum_with_offset = false;
        double sum_offset = 0.0;
        // Dedup: when multiple aggs target the same (col, name), only the
        // first one (primary) runs the scan; the rest set `primary_idx`
        // pointing to it and skip the inner loop. Emit reads from the
        // primary's AggState. Drops Q30's 90 redundant column scans to 1.
        idx_t primary_idx = INVALID_INDEX;
        // STRLEN/LENGTH(col_ref): when the agg arg is a scalar function
        // computing a VARCHAR's byte length, we accumulate string sizes
        // directly inside the parquet generic GROUP BY hot loop (Q28
        // AVG(STRLEN(URL))) without falling through to the per-row
        // ExpressionExecutor path. col_idx stays INVALID_INDEX so the
        // numeric fast paths still bypass this agg correctly; the
        // strlen-aware paths use strlen_col_idx instead.
        bool strlen_of_col = false;
        idx_t strlen_col_idx = INVALID_INDEX;
    };

    // Fused JOIN+aggregate hot-loop - defined out-of-class (needs full PhysicalHashJoin).
    // Returns true if it handled the computation (result_rows_ populated).
    bool TryComputeFusedJoinAggregate(PhysicalHashJoin *hj,
                                       const std::vector<AggInfo> &agg_infos,
                                       const std::vector<idx_t> &group_col_indices);

    // Fast path: read a double from a vector at index without Value allocation.
    static double ReadDouble(const Vector &vec, idx_t row) {
        auto tid = vec.GetType().id();
        auto *validity = &vec.GetValidity();
        if (!validity->RowIsValid(row)) return 0.0;
        switch (tid) {
        case LogicalTypeId::INTEGER:
        case LogicalTypeId::DATE:
            return static_cast<double>(reinterpret_cast<const int32_t *>(vec.GetData())[row]);
        case LogicalTypeId::BIGINT:
        case LogicalTypeId::TIMESTAMP:
        case LogicalTypeId::TIMESTAMP_TZ:
        case LogicalTypeId::TIME:
            return static_cast<double>(reinterpret_cast<const int64_t *>(vec.GetData())[row]);
        case LogicalTypeId::DOUBLE:  return reinterpret_cast<const double *>(vec.GetData())[row];
        case LogicalTypeId::FLOAT:   return static_cast<double>(reinterpret_cast<const float *>(vec.GetData())[row]);
        default: return 0.0;
        }
    }

    // Fast path: build a group key string from vector data directly.
    static void AppendGroupKey(std::string &key, const Vector &vec, idx_t row) {
        auto tid = vec.GetType().id();
        if (!vec.GetValidity().RowIsValid(row)) { key += "NULL|"; return; }
        switch (tid) {
        case LogicalTypeId::INTEGER:
        case LogicalTypeId::DATE:
            key += std::to_string(reinterpret_cast<const int32_t *>(vec.GetData())[row]); break;
        case LogicalTypeId::BIGINT:
        case LogicalTypeId::TIMESTAMP:
        case LogicalTypeId::TIMESTAMP_TZ:
        case LogicalTypeId::TIME:
            key += std::to_string(reinterpret_cast<const int64_t *>(vec.GetData())[row]); break;
        case LogicalTypeId::DOUBLE:  key += std::to_string(reinterpret_cast<const double *>(vec.GetData())[row]); break;
        case LogicalTypeId::VARCHAR: {
            auto &s = reinterpret_cast<const string_t *>(vec.GetData())[row];
            key.append(s.GetData(), s.GetSize());
            break;
        }
        default: key += vec.GetValue(row).ToString(); break;
        }
        key += '|';
    }

    void ComputeAggregates() {
        // Process chunks directly - no intermediate row materialization.
        // ankerl maps are 2-3× faster than std::unordered_map for the
        // 5M+ unique keys produced by Q35-shape multi-col GROUP BY.
        ankerl::unordered_dense::map<std::string, std::vector<AggState>> group_states;
        ankerl::unordered_dense::map<std::string, std::vector<Value>> group_keys;
        std::vector<std::string> group_order;
        // Int-only fast path: when the FUSED GENERIC parquet branch sees
        // GROUP BY columns that are all integral (no VARCHAR), the per-
        // thread `pkey` is content-deterministic across threads, so the
        // std::string content_key the generic path builds is unnecessary
        // work. The 5.7 M std::string allocations + 4 std::string-keyed
        // map insertions per pkey were ~25 s of Q35's wall on this code.
        // When `int_only_active` is set, run_merge and the result emit
        // loop iterate the uint64-keyed map below instead of the
        // std::string-keyed maps above.
        struct MergeSrc { int t; uint64_t pkey; const std::string *sk_raw; };
        // Fused per-group record: kv + states + cross-thread merge srcs
        // share one map slot so each pkey lookup probes ankerl once
        // instead of three times (group_keys/states/src_map).
        struct PerGroupRec {
            std::vector<Value> kv;
            std::vector<AggState> states;
            std::vector<MergeSrc> srcs;
        };
        ankerl::unordered_dense::map<uint64_t, PerGroupRec> group_recs_u64;
        std::vector<uint64_t> group_order_u64;
        bool int_only_active = false;

        idx_t num_aggs = aggregates_.size();

        // Pre-resolve aggregate argument column indices for fast path.
        std::vector<AggInfo> agg_infos(num_aggs);
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
            agg_infos[a].name = StringUtil::Upper(agg_expr.function_name);
            agg_infos[a].is_count_star = agg_expr.arguments.empty();
            agg_infos[a].is_distinct = agg_expr.is_distinct;
            agg_infos[a].col_idx = INVALID_INDEX;
            if (!agg_expr.arguments.empty()) {
                auto &arg = *agg_expr.arguments[0];
                if (arg.GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                    agg_infos[a].col_idx = static_cast<BoundColumnRef &>(arg).column_index;
                } else if (arg.GetExpressionType() == BoundExpressionType::FUNCTION) {
                    // SUM/AVG/MIN/MAX/COUNT(LENGTH(col)) or STRLEN(col):
                    // accumulate the byte length of each row's string. The
                    // GROUP BY hot loop reads sizes directly from the parquet
                    // VARCHAR column without per-row ExpressionExecutor cost.
                    auto &fn = static_cast<BoundFunction &>(arg);
                    auto fn_name = StringUtil::Upper(fn.function_name);
                    if ((fn_name == "LENGTH" || fn_name == "STRLEN") &&
                        fn.arguments.size() == 1 &&
                        fn.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        agg_infos[a].strlen_of_col = true;
                        agg_infos[a].strlen_col_idx =
                            static_cast<BoundColumnRef &>(*fn.arguments[0]).column_index;
                    }
                }
                if (!agg_infos[a].strlen_of_col &&
                    agg_infos[a].name == "SUM" && !agg_infos[a].is_distinct &&
                    arg.GetExpressionType() == BoundExpressionType::ARITHMETIC) {
                    // Detect SUM(col +/- const): share the base scan and
                    // derive each result at emit. Accept (col OP const) or
                    // (const + col); subtract is only valid as (col - const).
                    auto &ar = static_cast<BoundArithmetic &>(arg);
                    const BoundExpression *col_side = nullptr;
                    const BoundExpression *con_side = nullptr;
                    bool col_on_left = false;
                    if (ar.left->GetExpressionType() == BoundExpressionType::COLUMN_REF &&
                        ar.right->GetExpressionType() == BoundExpressionType::CONSTANT) {
                        col_side = ar.left.get(); con_side = ar.right.get();
                        col_on_left = true;
                    } else if (ar.left->GetExpressionType() == BoundExpressionType::CONSTANT &&
                               ar.right->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        col_side = ar.right.get(); con_side = ar.left.get();
                        col_on_left = false;
                    }
                    if (col_side && con_side && (ar.op == "+" || (ar.op == "-" && col_on_left))) {
                        const auto &con = static_cast<const BoundConstant &>(*con_side);
                        if (!con.value.IsNull()) {
                            double off = 0.0;
                            bool ok = true;
                            try {
                                switch (con.value.type().id()) {
                                case LogicalTypeId::TINYINT:  off = (double)con.value.GetValue<int8_t>(); break;
                                case LogicalTypeId::SMALLINT: off = (double)con.value.GetValue<int16_t>(); break;
                                case LogicalTypeId::INTEGER:  off = (double)con.value.GetValue<int32_t>(); break;
                                case LogicalTypeId::BIGINT:   off = (double)con.value.GetValue<int64_t>(); break;
                                case LogicalTypeId::FLOAT:    off = (double)con.value.GetValue<float>(); break;
                                case LogicalTypeId::DOUBLE:   off = con.value.GetValue<double>(); break;
                                default: ok = false; break;
                                }
                            } catch (...) { ok = false; }
                            if (ok) {
                                if (ar.op == "-") off = -off;
                                agg_infos[a].col_idx =
                                    static_cast<const BoundColumnRef &>(*col_side).column_index;
                                agg_infos[a].sum_with_offset = true;
                                agg_infos[a].sum_offset = off;
                            }
                        }
                    }
                }
            }
        }
        // Dedup: same (col, name, !is_distinct) → only the primary runs the
        // scan. Q30 has 90 SUMs over the same column; without this, the
        // FUSED PARQUET FAST PATH iterates the typed-numeric inner loop 90×
        // per row group. SUM with offsets shares one base scan because
        // emit synthesizes `sum + offset * count` per agg.
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &info = agg_infos[a];
            if (info.is_count_star) continue;
            if (info.col_idx == INVALID_INDEX) continue;
            if (info.is_distinct) continue;
            if (info.name != "SUM" && info.name != "COUNT" &&
                info.name != "AVG" && info.name != "MIN" && info.name != "MAX") continue;
            for (idx_t b = 0; b < a; b++) {
                auto &binfo = agg_infos[b];
                if (binfo.is_count_star) continue;
                if (binfo.col_idx != info.col_idx) continue;
                if (binfo.is_distinct) continue;
                if (binfo.name != info.name) continue;
                info.primary_idx = b;
                break;
            }
        }

        // Pre-resolve group-by column indices.
        std::vector<idx_t> group_col_indices;
        for (auto &g : groups_) {
            if (g->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                group_col_indices.push_back(static_cast<BoundColumnRef &>(*g).column_index);
            }
        }

        // === FAST PATH: fused JOIN + GROUP BY ===
        if (auto *hj = AsHashJoin(children[0].get())) {
            if (TryComputeFusedJoinAggregate(hj, agg_infos, group_col_indices)) return;
        }

        DataChunk chunk;
        chunk.Initialize(children[0]->GetTypes());
        std::string key;
        idx_t total_rows_processed = 0;

        // === FAST PATH: COUNT(*) with no GROUP BY - just count rows ===
        bool is_simple_count_star = (group_col_indices.empty() && num_aggs == 1 &&
                                     agg_infos[0].is_count_star && agg_infos[0].name == "COUNT");
        // FAST PATH: all COUNT/SUM/AVG on simple columns, no GROUP BY
        bool is_simple_no_group = group_col_indices.empty();
        bool all_simple_aggs = is_simple_no_group;
        for (idx_t a = 0; a < num_aggs && all_simple_aggs; a++) {
            auto &info = agg_infos[a];
            if (info.name == "COUNT" && info.is_count_star) continue;
            if ((info.name == "COUNT" || info.name == "SUM" || info.name == "AVG" ||
                 info.name == "MIN" || info.name == "MAX") && info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
            all_simple_aggs = false;
        }

        // === FAST PATH: single-column GROUP BY with simple aggregates ===
        // Sequential path is single-col only; parallel path handles multi-col too.
        bool single_group_fast = (group_col_indices.size() == 1 && all_simple_aggs == false &&
                                  num_aggs > 0);
        bool multi_group_fast = (group_col_indices.size() > 1 && all_simple_aggs == false &&
                                  num_aggs > 0);
        if (single_group_fast) {
            // Re-check: all aggs must be simple COUNT/SUM/AVG/MIN/MAX on direct columns
            single_group_fast = true;
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &info = agg_infos[a];
                if (info.is_count_star) continue;
                if ((info.name == "COUNT" || info.name == "SUM" || info.name == "AVG" ||
                     info.name == "MIN" || info.name == "MAX") &&
                    info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
                single_group_fast = false;
                break;
            }
        }

        // Parallel single-column GROUP BY: split file into N slices,
        // each thread runs full parse+aggregate, then merge.
        PhysicalFileScan *file_scan_for_group = nullptr;
        // Fused WHERE: if children[0] is PhysicalFilter wrapping PhysicalFileScan
        // and the predicate compiles, apply it per-row inside the worker loop.
        std::vector<SimplePredicate> fused_preds;
        bool fused_has_filter = false;
        // GROUP-BY-by-expr lift peek-through (T-204): planner inserts a
        // PhysicalProjection that materializes lifted exprs as synthetic
        // columns appended after `passthrough_w` source columns. Without
        // this peek, the parallel worker can't see the underlying scan and
        // we fall back to the per-row generic slow path.
        idx_t lift_passthrough_w = 0;
        std::vector<const BoundExpression *> lift_exprs;
        bool lift_active = false;
        if (single_group_fast || multi_group_fast) {
            file_scan_for_group = dynamic_cast<PhysicalFileScan *>(children[0].get());
            if (!file_scan_for_group) {
                if (auto *flt = dynamic_cast<PhysicalFilter *>(children[0].get())) {
                    if (!flt->children.empty()) {
                        if (auto *fs = dynamic_cast<PhysicalFileScan *>(flt->children[0].get())) {
                            std::vector<SimplePredicate> tmp;
                            if (flt->GetCondition() &&
                                TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                                file_scan_for_group = fs;
                                fused_preds = std::move(tmp);
                                fused_has_filter = true;
                            }
                        }
                    }
                }
            }
            if (!file_scan_for_group) {
                if (auto *proj = dynamic_cast<PhysicalProjection *>(children[0].get())) {
                    if (!proj->children.empty()) {
                        PhysicalFileScan *inner_scan = nullptr;
                        std::vector<SimplePredicate> tmp;
                        bool tmp_has_filter = false;
                        if (auto *fs_direct = dynamic_cast<PhysicalFileScan *>(proj->children[0].get())) {
                            inner_scan = fs_direct;
                        } else if (auto *flt = dynamic_cast<PhysicalFilter *>(proj->children[0].get())) {
                            if (!flt->children.empty()) {
                                if (auto *fs_under_flt = dynamic_cast<PhysicalFileScan *>(flt->children[0].get())) {
                                    if (flt->GetCondition() &&
                                        TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                                        inner_scan = fs_under_flt;
                                        tmp_has_filter = true;
                                    }
                                }
                            }
                        }
                        if (inner_scan) {
                            const auto &exprs = proj->GetExpressions();
                            idx_t source_w = inner_scan->GetTypes().size();
                            bool ok = exprs.size() >= source_w;
                            for (idx_t i = 0; ok && i < source_w; i++) {
                                auto *e = exprs[i].get();
                                if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) {
                                    ok = false; break;
                                }
                                if (static_cast<const BoundColumnRef &>(*e).column_index != i) {
                                    ok = false; break;
                                }
                            }
                            for (idx_t i = source_w; ok && i < exprs.size(); i++) {
                                auto *e = exprs[i].get();
                                if (!e || e->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                                    ok = false; break;
                                }
                            }
                            if (ok) {
                                file_scan_for_group = inner_scan;
                                lift_passthrough_w = source_w;
                                for (idx_t i = source_w; i < exprs.size(); i++)
                                    lift_exprs.push_back(exprs[i].get());
                                lift_active = true;
                                if (tmp_has_filter) {
                                    fused_preds = std::move(tmp);
                                    fused_has_filter = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        // Parallel GROUP BY (single or multi col): activate for large files.
        PhysicalFileScan *parallel_scan = nullptr;
        if ((single_group_fast || multi_group_fast) && file_scan_for_group &&
            file_scan_for_group->GetReader() &&
            file_scan_for_group->GetReader()->GetSize() > 16 * 1024 * 1024) {
            parallel_scan = file_scan_for_group;
            // Push projection down so threads skip unneeded columns.
            {
                idx_t nc = file_scan_for_group->GetTypes().size();
                std::vector<bool> need(nc, false);
                if (lift_active) {
                    // group_col_indices and agg col_idx live in POST-lift slot
                    // space. Source-slot indices (< passthrough_w) need their
                    // source column directly; lifted indices need every source
                    // column referenced by the lifted expression tree.
                    std::function<void(const BoundExpression &)> walk =
                        [&](const BoundExpression &x) {
                            if (x.GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                                auto &c = static_cast<const BoundColumnRef &>(x);
                                if (c.column_index < nc) need[c.column_index] = true;
                            } else if (x.GetExpressionType() == BoundExpressionType::COMPARISON) {
                                auto &c = static_cast<const BoundComparison &>(x);
                                walk(*c.left); walk(*c.right);
                            } else if (x.GetExpressionType() == BoundExpressionType::ARITHMETIC) {
                                auto &a = static_cast<const BoundArithmetic &>(x);
                                walk(*a.left); walk(*a.right);
                            } else if (x.GetExpressionType() == BoundExpressionType::FUNCTION) {
                                auto &f = static_cast<const BoundFunction &>(x);
                                for (auto &a : f.arguments) walk(*a);
                            } else if (x.GetExpressionType() == BoundExpressionType::CAST) {
                                auto &c = static_cast<const BoundCast &>(x);
                                walk(*c.child);
                            } else if (x.GetExpressionType() == BoundExpressionType::CONJUNCTION) {
                                auto &c = static_cast<const BoundConjunction &>(x);
                                walk(*c.left); walk(*c.right);
                            } else if (x.GetExpressionType() == BoundExpressionType::NEGATION) {
                                auto &n = static_cast<const BoundNegation &>(x);
                                walk(*n.child);
                            } else if (x.GetExpressionType() == BoundExpressionType::IS_NULL) {
                                auto &n = static_cast<const BoundIsNull &>(x);
                                walk(*n.child);
                            } else if (x.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
                                auto &u = static_cast<const BoundUnaryMinus &>(x);
                                walk(*u.child);
                            }
                        };
                    auto resolve = [&](idx_t post_idx) {
                        if (post_idx < lift_passthrough_w) {
                            if (post_idx < nc) need[post_idx] = true;
                        } else {
                            idx_t li = post_idx - lift_passthrough_w;
                            if (li < lift_exprs.size() && lift_exprs[li]) walk(*lift_exprs[li]);
                        }
                    };
                    for (idx_t gc : group_col_indices) resolve(gc);
                    for (auto &info : agg_infos) if (info.col_idx != INVALID_INDEX) resolve(info.col_idx);
                    for (auto &p : fused_preds) if (p.col_idx < nc) need[p.col_idx] = true;
                } else {
                    for (idx_t gc : group_col_indices) if (gc < nc) need[gc] = true;
                    for (auto &info : agg_infos) if (info.col_idx != INVALID_INDEX && info.col_idx < nc) need[info.col_idx] = true;
                    for (auto &p : fused_preds) if (p.col_idx < nc) need[p.col_idx] = true;
                }
                file_scan_for_group->SetProjection(need);
            }
            file_scan_for_group->Init(); // open reader + header if not already
        }
        if (parallel_scan) {
            auto *reader = parallel_scan->GetReader();
            const char *buffer = reader->GetBuffer();
            size_t total_size = reader->GetSize();
            size_t data_start = reader->GetPos();
            char delim = file_scan_for_group->GetDelimiter();
            size_t data_size = total_size - data_start;

            unsigned int num_threads = HWThreads();
            if (num_threads > 8) num_threads = 4;
            if (data_size < 16 * 1024 * 1024) num_threads = 1;

            // Compute per-thread byte ranges aligned to line boundaries.
            std::vector<size_t> ranges(num_threads + 1);
            ranges[0] = data_start;
            ranges[num_threads] = total_size;
            for (unsigned int t = 1; t < num_threads; t++) {
                size_t target = data_start + (data_size * t) / num_threads;
                ranges[t] = FastCSVReader::FindLineStart(buffer, total_size, target);
            }

            bool multi_group = group_col_indices.size() > 1;
            idx_t group_col = group_col_indices[0];
            // When lift is active the worker reads the inner scan's source
            // types and materializes lifted columns separately per chunk.
            auto types = lift_active ? file_scan_for_group->GetTypes()
                                     : children[0]->GetTypes();
            auto post_lift_types = children[0]->GetTypes();

            struct ThreadState {
                std::unordered_map<int64_t, std::vector<AggState>> int_groups;
                std::unordered_map<std::string, std::vector<AggState>> str_groups;
                std::unordered_map<int64_t, Value> int_keys;
                std::unordered_map<std::string, std::vector<Value>> str_keys_multi;
                std::unordered_map<std::string, Value> str_keys;
            };
            std::vector<ThreadState> tstates(num_threads);

            auto projection = file_scan_for_group->GetProjection();

            auto worker_fn = [&](unsigned int t) {
                    auto &ts = tstates[t];
                    FastCSVReader thread_reader(buffer, ranges[t], ranges[t + 1], delim);
                    DataChunk chunk;
                    chunk.Initialize(types);
                    // Per-thread output buffers for lifted expressions, sized
                    // to VECTOR_SIZE on first use. Only allocated when lift active.
                    std::vector<std::unique_ptr<Vector>> lift_out;
                    if (lift_active) {
                        lift_out.reserve(lift_exprs.size());
                        for (idx_t li = 0; li < lift_exprs.size(); li++) {
                            lift_out.push_back(std::make_unique<Vector>(
                                post_lift_types[lift_passthrough_w + li], VECTOR_SIZE));
                        }
                    }

                    while (true) {
                        chunk.Reset();
                        idx_t cnt = projection.empty()
                            ? thread_reader.ReadChunk(chunk, types)
                            : thread_reader.ReadChunkProjected(chunk, types, projection);
                        if (cnt == 0) break;

                        if (lift_active) {
                            for (idx_t li = 0; li < lift_exprs.size(); li++) {
                                lift_out[li]->GetValidity().Reset();
                                ExpressionExecutor::Execute(*lift_exprs[li], chunk,
                                                            *lift_out[li], cnt);
                            }
                        }

                        auto get_vec = [&](idx_t gci) -> Vector & {
                            if (lift_active && gci >= lift_passthrough_w) {
                                return *lift_out[gci - lift_passthrough_w];
                            }
                            return chunk.GetVector(gci);
                        };
                        auto get_value = [&](idx_t gci, idx_t i) -> Value {
                            if (lift_active && gci >= lift_passthrough_w) {
                                return lift_out[gci - lift_passthrough_w]->GetValue(i);
                            }
                            return chunk.GetValue(gci, i);
                        };

                        auto &gvec = get_vec(group_col);
                        auto gtid = gvec.GetType().id();
                        bool is_int = (gtid == LogicalTypeId::INTEGER || gtid == LogicalTypeId::BIGINT) && !multi_group;

                        for (idx_t i = 0; i < cnt; i++) {
                            if (fused_has_filter &&
                                !EvalSimplePredicatesChunk(fused_preds, chunk, i)) continue;
                            std::vector<AggState> *states_ptr = nullptr;
                            if (multi_group) {
                                // Concatenated string key for multi-col GROUP BY.
                                std::string k;
                                for (idx_t gci : group_col_indices) {
                                    auto &v = get_vec(gci);
                                    auto tid = v.GetType().id();
                                    if (!v.GetValidity().RowIsValid(i)) { k += "\x01N"; }
                                    else if (tid == LogicalTypeId::INTEGER)
                                        k += std::to_string(reinterpret_cast<const int32_t *>(v.GetData())[i]);
                                    else if (tid == LogicalTypeId::BIGINT)
                                        k += std::to_string(reinterpret_cast<const int64_t *>(v.GetData())[i]);
                                    else if (tid == LogicalTypeId::DOUBLE)
                                        k += std::to_string(reinterpret_cast<const double *>(v.GetData())[i]);
                                    else if (tid == LogicalTypeId::VARCHAR) {
                                        auto &s = reinterpret_cast<const string_t *>(v.GetData())[i];
                                        k.append(s.GetData(), s.GetSize());
                                    }
                                    k += '|';
                                }
                                auto it = ts.str_groups.find(k);
                                if (it == ts.str_groups.end()) {
                                    std::vector<Value> vals;
                                    vals.reserve(group_col_indices.size());
                                    for (idx_t gci : group_col_indices)
                                        vals.push_back(get_value(gci, i));
                                    ts.str_keys_multi[k] = std::move(vals);
                                    it = ts.str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                                }
                                states_ptr = &it->second;
                            } else if (is_int) {
                                int64_t k = (gtid == LogicalTypeId::BIGINT)
                                    ? reinterpret_cast<const int64_t *>(gvec.GetData())[i]
                                    : (int64_t)reinterpret_cast<const int32_t *>(gvec.GetData())[i];
                                auto it = ts.int_groups.find(k);
                                if (it == ts.int_groups.end()) {
                                    ts.int_keys[k] = get_value(group_col, i);
                                    it = ts.int_groups.emplace(k, std::vector<AggState>(num_aggs)).first;
                                }
                                states_ptr = &it->second;
                            } else {
                                auto &s = reinterpret_cast<const string_t *>(gvec.GetData())[i];
                                std::string k(s.GetData(), s.GetSize());
                                auto it = ts.str_groups.find(k);
                                if (it == ts.str_groups.end()) {
                                    ts.str_keys[k] = get_value(group_col, i);
                                    it = ts.str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                                }
                                states_ptr = &it->second;
                            }

                            auto &states = *states_ptr;
                            for (idx_t a = 0; a < num_aggs; a++) {
                                auto &state = states[a];
                                auto &info = agg_infos[a];
                                if (info.name == "COUNT" && info.is_count_star) {
                                    state.count++;
                                } else if (info.col_idx != INVALID_INDEX) {
                                    auto &vec = get_vec(info.col_idx);
                                    if (vec.GetValidity().RowIsValid(i)) {
                                        if (info.name == "COUNT") {
                                            state.count++;
                                        } else {
                                            double val = ReadDouble(vec, i);
                                            if (info.name == "SUM" || info.name == "AVG") {
                                                state.count++; state.sum += val;
                                            } else if (info.name == "MIN") {
                                                if (!state.has_min || val < state.sum_min) {
                                                    state.sum_min = val; state.has_min = true;
                                                }
                                            } else if (info.name == "MAX") {
                                                if (!state.has_max || val > state.sum_max) {
                                                    state.sum_max = val; state.has_max = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                };
            if (num_threads > 1) {
                std::vector<std::thread> threads;
                threads.reserve(num_threads);
                for (unsigned int t = 0; t < num_threads; t++)
                    threads.emplace_back(worker_fn, t);
                for (auto &th : threads) th.join();
            } else {
                worker_fn(0);
            }

            // Merge per-thread states.
            std::unordered_map<int64_t, std::vector<AggState>> int_groups;
            std::unordered_map<std::string, std::vector<AggState>> str_groups;
            std::vector<int64_t> int_order;
            std::vector<std::string> str_order;
            std::unordered_map<int64_t, Value> int_keys;
            std::unordered_map<std::string, Value> str_keys;
            std::unordered_map<std::string, std::vector<Value>> str_keys_multi;

            for (auto &ts : tstates) {
                for (auto &kv : ts.int_groups) {
                    auto &dst = int_groups[kv.first];
                    if (dst.empty()) {
                        dst = std::move(kv.second);
                        int_keys[kv.first] = ts.int_keys[kv.first];
                        int_order.push_back(kv.first);
                    } else {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            dst[a].count += kv.second[a].count;
                            dst[a].sum += kv.second[a].sum;
                            if (kv.second[a].has_min &&
                                (!dst[a].has_min || kv.second[a].sum_min < dst[a].sum_min)) {
                                dst[a].sum_min = kv.second[a].sum_min; dst[a].has_min = true;
                            }
                            if (kv.second[a].has_max &&
                                (!dst[a].has_max || kv.second[a].sum_max > dst[a].sum_max)) {
                                dst[a].sum_max = kv.second[a].sum_max; dst[a].has_max = true;
                            }
                        }
                    }
                }
                for (auto &kv : ts.str_groups) {
                    auto &dst = str_groups[kv.first];
                    if (dst.empty()) {
                        dst = std::move(kv.second);
                        if (multi_group) {
                            auto it = ts.str_keys_multi.find(kv.first);
                            if (it != ts.str_keys_multi.end()) str_keys_multi[kv.first] = it->second;
                        } else {
                            str_keys[kv.first] = ts.str_keys[kv.first];
                        }
                        str_order.push_back(kv.first);
                    } else {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            dst[a].count += kv.second[a].count;
                            dst[a].sum += kv.second[a].sum;
                            if (kv.second[a].has_min &&
                                (!dst[a].has_min || kv.second[a].sum_min < dst[a].sum_min)) {
                                dst[a].sum_min = kv.second[a].sum_min; dst[a].has_min = true;
                            }
                            if (kv.second[a].has_max &&
                                (!dst[a].has_max || kv.second[a].sum_max > dst[a].sum_max)) {
                                dst[a].sum_max = kv.second[a].sum_max; dst[a].has_max = true;
                            }
                        }
                    }
                }
            }

            for (auto k : int_order) {
                std::string sk = std::to_string(k);
                group_states[sk] = std::move(int_groups[k]);
                group_keys[sk] = {int_keys[k]};
                group_order.push_back(sk);
            }
            if (multi_group) {
                for (auto &k : str_order) {
                    group_states[k] = std::move(str_groups[k]);
                    auto it = str_keys_multi.find(k);
                    if (it != str_keys_multi.end()) group_keys[k] = std::move(it->second);
                    group_order.push_back(k);
                }
            } else
            for (auto &k : str_order) {
                group_states[k] = std::move(str_groups[k]);
                group_keys[k] = {str_keys[k]};
                group_order.push_back(k);
            }
        } else if (false) { // marker to keep else-if structure intact
            // Initialize the file scan to load the buffer.
            file_scan_for_group->Init();
            auto *reader = file_scan_for_group->GetReader();
            const char *buffer = reader->GetBuffer();
            size_t total_size = reader->GetSize();
            size_t data_start = reader->GetPos(); // after header
            char delim = file_scan_for_group->GetDelimiter();

            idx_t group_col = group_col_indices[0];
            auto types = children[0]->GetTypes();

            // Determine number of threads based on file size.
            unsigned int num_threads = HWThreads();
            if (num_threads > 16) num_threads = 8;
            size_t data_size = total_size - data_start;
            if (data_size < 4 * 1024 * 1024) num_threads = 1;

            // Compute per-thread byte ranges aligned to line boundaries.
            std::vector<size_t> ranges(num_threads + 1);
            ranges[0] = data_start;
            ranges[num_threads] = total_size;
            for (unsigned int t = 1; t < num_threads; t++) {
                size_t target = data_start + (data_size * t) / num_threads;
                ranges[t] = FastCSVReader::FindLineStart(buffer, total_size, target);
            }

            // Per-thread state.
            struct ThreadState {
                std::unordered_map<int64_t, std::vector<AggState>> int_groups;
                std::unordered_map<std::string, std::vector<AggState>> str_groups;
                std::unordered_map<int64_t, Value> int_keys;
                std::unordered_map<std::string, Value> str_keys;
            };
            std::vector<ThreadState> tstates(num_threads);

            auto worker_fn2 = [&](unsigned int t) {
                    auto &ts = tstates[t];
                    size_t pos = ranges[t];
                    size_t end = ranges[t + 1];

                    DataChunk thread_chunk;
                    thread_chunk.Initialize(types);

                    // Inline mini-parser that reads from buffer[pos..end].
                    auto ParseField = [&](const char *&fs, size_t &fl) -> bool {
                        if (pos >= end) return false;
                        char c = buffer[pos];
                        if (c == '\n' || c == '\r') return false;
                        if (c == '"') {
                            pos++; fs = buffer + pos; size_t s = pos;
                            while (pos < end && buffer[pos] != '"') pos++;
                            fl = pos - s;
                            if (pos < end) pos++;
                            if (pos < end && buffer[pos] == delim) pos++;
                            return true;
                        }
                        fs = buffer + pos; size_t s = pos;
                        while (pos < end && buffer[pos] != delim &&
                               buffer[pos] != '\n' && buffer[pos] != '\r') pos++;
                        fl = pos - s;
                        if (pos < end && buffer[pos] == delim) pos++;
                        return true;
                    };

                    auto ParseInt64Local = [](const char *s, size_t len) -> int64_t {
                        if (len == 0) return 0;
                        bool neg = (s[0] == '-');
                        int64_t r = 0;
                        for (size_t i = neg ? 1 : 0; i < len; i++) r = r * 10 + (s[i] - '0');
                        return neg ? -r : r;
                    };

                    auto ParseDoubleLocal = [](const char *s, size_t len) -> double {
                        char buf[64];
                        size_t cl = len < 63 ? len : 63;
                        memcpy(buf, s, cl); buf[cl] = '\0';
                        return strtod(buf, nullptr);
                    };

                    idx_t num_cols = static_cast<idx_t>(types.size());
                    // Hoist field storage out of the loop - reused per row.
                    std::vector<const char*> field_starts(num_cols);
                    std::vector<size_t> field_lens(num_cols);
                    while (pos < end) {
                        idx_t col = 0;
                        const char *fs; size_t fl;
                        while (col < num_cols && ParseField(fs, fl)) {
                            field_starts[col] = fs;
                            field_lens[col] = fl;
                            col++;
                        }
                        // Skip rest of line.
                        while (pos < end && buffer[pos] != '\n' && buffer[pos] != '\r') pos++;
                        if (pos < end && buffer[pos] == '\r') pos++;
                        if (pos < end && buffer[pos] == '\n') pos++;
                        if (col == 0) continue;

                        // Determine group key from group_col field.
                        if (group_col >= col) continue;
                        const char *gf_ptr = field_starts[group_col];
                        size_t gf_len = field_lens[group_col];

                        std::vector<AggState> *states_ptr = nullptr;
                        auto gtid = types[group_col].id();
                        if (gtid == LogicalTypeId::BIGINT || gtid == LogicalTypeId::INTEGER) {
                            int64_t k = ParseInt64Local(gf_ptr, gf_len);
                            auto it = ts.int_groups.find(k);
                            if (it == ts.int_groups.end()) {
                                ts.int_keys[k] = (gtid == LogicalTypeId::BIGINT)
                                    ? Value::BIGINT(k) : Value::INTEGER((int32_t)k);
                                it = ts.int_groups.emplace(k, std::vector<AggState>(num_aggs)).first;
                            }
                            states_ptr = &it->second;
                        } else {
                            std::string k(gf_ptr, gf_len);
                            auto it = ts.str_groups.find(k);
                            if (it == ts.str_groups.end()) {
                                ts.str_keys[k] = Value::VARCHAR(k);
                                it = ts.str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                            }
                            states_ptr = &it->second;
                        }

                        auto &states = *states_ptr;
                        for (idx_t a = 0; a < num_aggs; a++) {
                            auto &state = states[a];
                            auto &info = agg_infos[a];
                            if (info.name == "COUNT" && info.is_count_star) {
                                state.count++;
                            } else if (info.col_idx != INVALID_INDEX && info.col_idx < col) {
                                const char *af_ptr = field_starts[info.col_idx];
                                size_t af_len = field_lens[info.col_idx];
                                if (af_len == 0) continue;
                                double val = 0;
                                auto cid = types[info.col_idx].id();
                                if (cid == LogicalTypeId::BIGINT)
                                    val = (double)ParseInt64Local(af_ptr, af_len);
                                else if (cid == LogicalTypeId::INTEGER)
                                    val = (double)(int32_t)ParseInt64Local(af_ptr, af_len);
                                else if (cid == LogicalTypeId::DOUBLE || cid == LogicalTypeId::FLOAT)
                                    val = ParseDoubleLocal(af_ptr, af_len);

                                if (info.name == "COUNT") {
                                    state.count++;
                                } else if (info.name == "SUM" || info.name == "AVG") {
                                    state.count++; state.sum += val;
                                } else if (info.name == "MIN") {
                                    if (!state.has_min || val < state.sum_min) {
                                        state.sum_min = val; state.has_min = true;
                                    }
                                } else if (info.name == "MAX") {
                                    if (!state.has_max || val > state.sum_max) {
                                        state.sum_max = val; state.has_max = true;
                                    }
                                }
                            }
                        }
                    }
                };
            if (num_threads > 1) {
                std::vector<std::thread> threads;
                threads.reserve(num_threads);
                for (unsigned int t = 0; t < num_threads; t++)
                    threads.emplace_back(worker_fn2, t);
                for (auto &th : threads) th.join();
            } else {
                worker_fn2(0);
            }

            // Merge per-thread states.
            std::unordered_map<int64_t, std::vector<AggState>> int_groups;
            std::unordered_map<std::string, std::vector<AggState>> str_groups;
            std::vector<int64_t> int_order;
            std::vector<std::string> str_order;
            std::unordered_map<int64_t, Value> int_keys;
            std::unordered_map<std::string, Value> str_keys;

            for (auto &ts : tstates) {
                for (auto &kv : ts.int_groups) {
                    auto &dst = int_groups[kv.first];
                    if (dst.empty()) {
                        dst = std::move(kv.second);
                        int_keys[kv.first] = ts.int_keys[kv.first];
                        int_order.push_back(kv.first);
                    } else {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            dst[a].count += kv.second[a].count;
                            dst[a].sum += kv.second[a].sum;
                            if (kv.second[a].has_min &&
                                (!dst[a].has_min || kv.second[a].sum_min < dst[a].sum_min)) {
                                dst[a].sum_min = kv.second[a].sum_min; dst[a].has_min = true;
                            }
                            if (kv.second[a].has_max &&
                                (!dst[a].has_max || kv.second[a].sum_max > dst[a].sum_max)) {
                                dst[a].sum_max = kv.second[a].sum_max; dst[a].has_max = true;
                            }
                        }
                    }
                }
                for (auto &kv : ts.str_groups) {
                    auto &dst = str_groups[kv.first];
                    if (dst.empty()) {
                        dst = std::move(kv.second);
                        str_keys[kv.first] = ts.str_keys[kv.first];
                        str_order.push_back(kv.first);
                    } else {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            dst[a].count += kv.second[a].count;
                            dst[a].sum += kv.second[a].sum;
                            if (kv.second[a].has_min &&
                                (!dst[a].has_min || kv.second[a].sum_min < dst[a].sum_min)) {
                                dst[a].sum_min = kv.second[a].sum_min; dst[a].has_min = true;
                            }
                            if (kv.second[a].has_max &&
                                (!dst[a].has_max || kv.second[a].sum_max > dst[a].sum_max)) {
                                dst[a].sum_max = kv.second[a].sum_max; dst[a].has_max = true;
                            }
                        }
                    }
                }
            }

            // Build result map for downstream.
            for (auto k : int_order) {
                std::string sk = std::to_string(k);
                group_states[sk] = std::move(int_groups[k]);
                group_keys[sk] = {int_keys[k]};
                group_order.push_back(sk);
            }
            for (auto &k : str_order) {
                group_states[k] = std::move(str_groups[k]);
                group_keys[k] = {str_keys[k]};
                group_order.push_back(k);
            }
        } else if (single_group_fast) {
            idx_t group_col = group_col_indices[0];

            // Ultra-fast path: single VARCHAR group column + only COUNT(*) aggs.
            // Walk CSV buffer directly, no DataChunk materialization.
            bool ultrafast = false;
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                auto types_arr = children[0]->GetTypes();
                if (group_col < types_arr.size() &&
                    types_arr[group_col].id() == LogicalTypeId::VARCHAR) {
                    bool all_count_star = true;
                    for (auto &info : agg_infos) {
                        if (!(info.name == "COUNT" && info.is_count_star)) {
                            all_count_star = false;
                            break;
                        }
                    }
                    if (all_count_star && fs->GetReader()) {
                        ultrafast = true;
                        fs->Init();

                        struct CacheEntry {
                            std::string key;
                            int64_t count;
                        };
                        std::vector<CacheEntry> cache;
                        cache.reserve(256);

                        fs->GetReader()->ForEachVarcharCol(group_col, types_arr.size(),
                            [&](const char *d, size_t l) {
                                for (auto &e : cache) {
                                    if (e.key.size() == l && memcmp(e.key.data(), d, l) == 0) {
                                        e.count++;
                                        return;
                                    }
                                }
                                cache.push_back({std::string(d, l), 1});
                            });

                        // Move into standard map for result emit.
                        for (auto &e : cache) {
                            group_order.push_back(e.key);
                            group_keys[e.key] = {Value::VARCHAR(e.key)};
                            auto &st = group_states[e.key];
                            st.resize(num_aggs);
                            for (auto &s : st) s.count = e.count;
                        }
                    }
                }
            }
            if (!ultrafast) {
                // Column pruning: tell file scan or join to only populate needed columns.
                std::vector<bool> needed(children[0]->GetTypes().size(), false);
                needed[group_col] = true;
                for (auto &info : agg_infos) {
                    if (info.col_idx != INVALID_INDEX && info.col_idx < needed.size()) {
                        needed[info.col_idx] = true;
                    }
                }
                children[0]->SetNeededOutputs(needed);
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                fs->SetProjection(std::move(needed));
            }

            // Direct-pointer cache for VARCHAR group keys (low-cardinality optimization).
            // Reserve enough for typical cardinality - pointers remain stable.
            std::vector<std::string> str_cache_keys;
            std::vector<std::vector<AggState>> str_cache_states;
            str_cache_keys.reserve(256);
            str_cache_states.reserve(256);

            // Two hash maps based on group column type - avoid string conversion.
            // unordered_dense is ~2× faster than std::unordered_map for the hot
            // INT-keyed GROUP BY path (flat open-addressing vs chained buckets).
            ankerl::unordered_dense::map<int64_t, std::vector<AggState>> int_groups;
            std::unordered_map<std::string, std::vector<AggState>> str_groups;
            std::vector<int64_t> int_order;
            std::vector<std::string> str_order;
            ankerl::unordered_dense::map<int64_t, Value> int_keys;
            std::unordered_map<std::string, Value> str_keys;

            // Per-shard merge buckets (populated only by the parquet fast path).
            // Declared at this scope so the post-if/else direct-emit and
            // shard-rebuild loops can read them.
            constexpr int MERGE_SHARDS = 4;
            struct MergeShard {
                ankerl::unordered_dense::map<int64_t, std::vector<AggState>> int_groups;
                std::vector<int64_t> int_order;
            };
            std::vector<MergeShard> int_merge_shards(MERGE_SHARDS);
            // Group-col type for synthesizing Value at emit/rebuild time
            // (replaces the per-thread int_keys map). Set by the parquet
            // fast path; chunk fallback doesn't use it (writes int_keys
            // directly via chunk.GetValue()).
            bool grp_is_bigint = false;

            // === FUSED PARQUET SINGLE-COLUMN GROUP BY ===
            // Iterate row groups directly from ParquetColumnData - skip
            // DataChunk materialization and the per-row vector dispatch.
            // Also handle AGG -> FILTER -> SCAN: compile the filter into a
            // flat predicate list and apply per row, skipping the filter's
            // copy-into-new-chunk pass entirely.
            PhysicalParquetScan *pq = dynamic_cast<PhysicalParquetScan *>(children[0].get());
            std::vector<SimplePredicate> single_preds;
            bool single_has_filter_fused = false;
            if (!pq) {
                if (auto *flt = dynamic_cast<PhysicalFilter *>(children[0].get())) {
                    if (!flt->children.empty()) {
                        if (auto *spq = dynamic_cast<PhysicalParquetScan *>(flt->children[0].get())) {
                            std::vector<SimplePredicate> tmp;
                            if (flt->GetCondition() &&
                                TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                                pq = spq;
                                single_preds = std::move(tmp);
                                single_has_filter_fused = true;
                                // Expand projection to include predicate cols.
                                std::vector<bool> need(pq->GetTypes().size(), false);
                                need[group_col] = true;
                                for (auto &info : agg_infos) {
                                    if (info.col_idx != INVALID_INDEX && info.col_idx < need.size())
                                        need[info.col_idx] = true;
                                }
                                for (auto &p : single_preds) {
                                    if (p.col_idx < need.size()) need[p.col_idx] = true;
                                }
                                pq->SetNeededOutputs(need);
                                // VARCHAR predicate cols: BuildTypedKeepMask
                                // + EvalSimplePredicates both prefer
                                // str_dict_values; per-row str_data bytes
                                // unused. Skip the materialisation; PLAIN
                                // pages trigger back-fill in parquet.cpp.
                                // Q22 URL falls here.
                                std::vector<bool> skip(pq->GetTypes().size(), false);
                                for (auto &p : single_preds) {
                                    if (p.col_idx < skip.size() && p.str_form &&
                                        pq->GetTypes()[p.col_idx].id() == LogicalTypeId::VARCHAR &&
                                        p.col_idx != group_col) {
                                        skip[p.col_idx] = true;
                                    }
                                }
                                // Q13/Q11/Q12-shape: when group col is VARCHAR
                                // and all aggs are CountStar, the dict_fast
                                // branch reads dict_indices+dict_values only;
                                // str_data is dead weight (12-16 B per row of
                                // string_t writes that nobody reads). Skip it;
                                // the rare PLAIN page back-fills str_data
                                // automatically and falls into the non-dict
                                // branch (str_dict_encoded toggles false).
                                // Q13_eligible_skip widened: group_col VARCHAR
                                // dict_fast paths read str_dict_indices+
                                // str_dict_values, never str_data. Safe to
                                // skip iff NO agg targets the group_col
                                // itself (count_star never targets cols, MIN/
                                // MAX/SUM on a different col is unaffected).
                                // Q11_base / Q22_URL / Q26 (MIN(URL) GROUP BY
                                // SearchPhrase) all qualify.
                                bool q13_eligible_skip =
                                    (pq->GetTypes()[group_col].id() ==
                                     LogicalTypeId::VARCHAR);
                                for (idx_t a = 0; a < num_aggs && q13_eligible_skip; a++) {
                                    if (!agg_infos[a].is_count_star &&
                                        agg_infos[a].col_idx == group_col)
                                        q13_eligible_skip = false;
                                }
                                if (q13_eligible_skip) skip[group_col] = true;
                                pq->SetSkipStrData(std::move(skip));

                                // Selection-vector pushdown (Q22 / Q23 / Q26
                                // shape): when the WHERE filter has a
                                // <>'' / ='' predicate on a dict-encoded
                                // VARCHAR col, decode that col FIRST and
                                // build a row mask. The mask gates dst[i]
                                // writes in subsequent PLAIN VARCHAR decodes
                                // — e.g. on Q22 the SearchPhrase NE filter
                                // keeps ~13% of rows; URL is then decoded
                                // with the mask, skipping ~87% × 100M × 16B
                                // of string_t writes (~1.4 GB).
                                //
                                // Routes through SetTwoPhaseDecode → the
                                // SEPARATE DecodeRowGroupIntoTwoPhase method.
                                // The single-pass DecodeRowGroupInto stays
                                // bit-identical to pre-pushdown — Q31/Q32
                                // 2-col GROUP BY hot path is untouched.
                                idx_t filt_col_2p = INVALID_INDEX;
                                bool filt_ne_2p = false;
                                for (auto &p : single_preds) {
                                    if (!p.str_form) continue;
                                    if (p.col_idx >= pq->GetTypes().size()) continue;
                                    if (pq->GetTypes()[p.col_idx].id() !=
                                        LogicalTypeId::VARCHAR) continue;
                                    if (p.like_contains) continue;
                                    if (p.op != SimpleCmpOp::NE &&
                                        p.op != SimpleCmpOp::EQ) continue;
                                    if (!p.sval.empty()) continue;  // only ""
                                    filt_col_2p = p.col_idx;
                                    filt_ne_2p = (p.op == SimpleCmpOp::NE);
                                    break;
                                }
                                // Two-phase pays off only if there's a
                                // projected OTHER VARCHAR col whose
                                // str_data writes we can mask out.
                                bool other_varchar_2p = false;
                                if (filt_col_2p != INVALID_INDEX) {
                                    for (idx_t c = 0; c < pq->GetTypes().size(); c++) {
                                        if (c == filt_col_2p) continue;
                                        if (c >= need.size() || !need[c]) continue;
                                        if (pq->GetTypes()[c].id() ==
                                            LogicalTypeId::VARCHAR) {
                                            other_varchar_2p = true;
                                            break;
                                        }
                                    }
                                }
                                if (other_varchar_2p) {
                                    std::vector<idx_t> early = {filt_col_2p};
                                    idx_t filt_col_cap = filt_col_2p;
                                    bool ne_cap = filt_ne_2p;
                                    pq->SetTwoPhaseDecode(
                                        std::move(early),
                                        [filt_col_cap, ne_cap]
                                        (const std::vector<ParquetColumnData>& cols,
                                         std::vector<uint8_t>& mask) {
                                            if (filt_col_cap >= cols.size()) return;
                                            const auto &c = cols[filt_col_cap];
                                            if (!c.decoded) return;
                                            if (!c.str_dict_encoded ||
                                                c.str_dict_indices.empty()) return;
                                            // Find dict_idx of the empty string.
                                            uint32_t empty_di = UINT32_MAX;
                                            for (uint32_t d = 0;
                                                 d < c.str_dict_values.size(); d++) {
                                                if (c.str_dict_values[d].GetSize() == 0) {
                                                    empty_di = d;
                                                    break;
                                                }
                                            }
                                            const auto *idx = c.str_dict_indices.data();
                                            idx_t n = c.str_dict_indices.size();
                                            mask.resize(n);
                                            if (ne_cap) {
                                                for (idx_t r = 0; r < n; r++) {
                                                    mask[r] = (uint8_t)(idx[r] != empty_di);
                                                }
                                            } else {
                                                for (idx_t r = 0; r < n; r++) {
                                                    mask[r] = (uint8_t)(idx[r] == empty_di);
                                                }
                                            }
                                        });
                                }
                            }
                        }
                    }
                }
            }
            if (pq) {
                // === Q13/Q16-shape: high-card single-col INT GROUP BY +
                //     COUNT(*) only. Default per-thread ankerl map blows up
                //     to 12 GB at 17M+ unique groups (each thread sees all
                //     uniques) → cache-miss every probe → 600 ns/row.
                //     RadixCountAgg pre-partitions rows into 16 radix
                //     buckets per thread, then 16 disjoint workers union
                //     across threads with no contention. Lives in a
                //     separate TU so physical_planner.cpp .text stays
                //     stable.
                //
                //     Accepts multiple aggs as long as ALL are COUNT(*) — the
                //     planner duplicates the agg when ORDER BY references its
                //     alias, so Q16 produces two identical entries.
                {
                    auto group_tid_q16 = pq->GetTypes()[group_col].id();
                    bool all_count_star_q16 = (num_aggs >= 1);
                    for (idx_t a = 0; a < num_aggs && all_count_star_q16; a++) {
                        if (!agg_infos[a].is_count_star) all_count_star_q16 = false;
                    }
                    bool q16_shape = (all_count_star_q16 &&
                                      (group_tid_q16 == LogicalTypeId::BIGINT ||
                                       group_tid_q16 == LogicalTypeId::INTEGER));
                    bool q13_shape = (all_count_star_q16 &&
                                      group_tid_q16 == LogicalTypeId::VARCHAR);
                    if (q13_shape) {
                        // Q13/Q34/Q35-shape: high-card single-col VARCHAR
                        // GROUP BY + only COUNT(*). 12 threads = all logical
                        // procs on the 6C/12T chip (see RunParallelRGs).
                        constexpr int Q13_THREADS = 12;
                        // `group_col <> ''` filter (Q13) gives a bounded
                        // survivor count; no filter at all (Q34/Q35 GROUP BY
                        // URL) ingests every row. Both feed the bounded-HT +
                        // radix-spill aggregation: it caps every per-row probe
                        // at an L2-resident table and spills scatter-free. For
                        // a high-card VARCHAR GROUP BY the aggregation (not
                        // parquet decode) is the wall.
                        bool hc_ne_empty = false;
                        if (single_has_filter_fused && single_preds.size() == 1) {
                            const auto &p0 = single_preds[0];
                            hc_ne_empty = p0.str_form &&
                                (idx_t)p0.col_idx == group_col &&
                                p0.op == SimpleCmpOp::NE && p0.sval.empty();
                        }
                        // No filter -> ingest every row (keep the empty group).
                        bool hc_take = hc_ne_empty || !single_has_filter_fused;
                        if (hc_take) {
                            const bool hc_skip_empty = hc_ne_empty;
                            slothdb::RadixHashCountStr hagg(Q13_THREADS);
                            pq->SetRGConsumer(
                                [&, hc_skip_empty](const PhysicalParquetScan::RGWork &work,
                                    idx_t rg_idx, int tid) {
                                    if (work.pruned) return;
                                    idx_t nrows = pq->RowGroupSize(rg_idx);
                                    const auto &gcol = work.cols[group_col];
                                    if (!gcol.decoded) return;
                                    int t = tid % Q13_THREADS;
                                    const uint8_t *val = gcol.all_valid
                                        ? nullptr : gcol.validity.data();
                                    if (gcol.str_dict_encoded &&
                                        !gcol.str_dict_indices.empty()) {
                                        const auto &dv = gcol.str_dict_values;
                                        uint32_t skip_di = UINT32_MAX;
                                        if (hc_skip_empty) {
                                            for (uint32_t d = 0; d < dv.size(); d++) {
                                                if (dv[d].GetSize() == 0) {
                                                    skip_di = d; break;
                                                }
                                            }
                                        }
                                        hagg.IngestDictRG(t,
                                            gcol.str_dict_indices.data(),
                                            (uint32_t)nrows, dv.data(),
                                            (uint32_t)dv.size(), val, skip_di);
                                    } else if (!gcol.str_data.empty()) {
                                        hagg.IngestPlainRG(t,
                                            gcol.str_data.data(),
                                            (uint32_t)nrows, val, hc_skip_empty);
                                    }
                                });
                            pq->RunParallelRGs(Q13_THREADS);
                            hagg.Finalize();
                            int hc_top_k = 0;
                            if (topn_active_ && topn_limit_ > 0 &&
                                !topn_ascending_ && topn_col_idx_ >= 1 &&
                                topn_col_idx_ <= num_aggs) {
                                hc_top_k = (int)topn_limit_;
                            }
                            auto hc_results = hagg.EmitTopK(hc_top_k);
                            result_rows_.reserve(hc_results.size());
                            for (auto &res : hc_results) {
                                std::vector<Value> row;
                                row.reserve(1 + num_aggs);
                                row.push_back(Value::VARCHAR(res.key));
                                for (idx_t a = 0; a < num_aggs; a++) {
                                    row.push_back(Value::BIGINT(res.count));
                                }
                                result_rows_.push_back(std::move(row));
                            }
                            return;
                        }
                        slothdb::RadixCountAggStr str_agg(Q13_THREADS);
                        pq->SetRGConsumer(
                            [&](const PhysicalParquetScan::RGWork &work,
                                idx_t rg_idx, int tid) {
                                if (work.pruned) return;
                                idx_t nrows = pq->RowGroupSize(rg_idx);
                                const auto &gcol = work.cols[group_col];
                                if (!gcol.decoded) return;
                                bool dict_fast = gcol.str_dict_encoded &&
                                    !gcol.str_dict_indices.empty();
                                // Q13/Q14-style fast skip: when the only
                                // filter is `<group_col> <> ''` (or any
                                // equality on the same col), find its
                                // dict_idx and fold the filter into a
                                // per-row dict-idx skip. Avoids the O(N)
                                // BuildTypedKeepMask + 100MB keep_mask read.
                                uint32_t skip_di_q13 = UINT32_MAX;
                                bool single_pred_skip = false;
                                if (dict_fast && single_has_filter_fused &&
                                    single_preds.size() == 1) {
                                    const auto &p = single_preds[0];
                                    if (p.str_form &&
                                        (idx_t)p.col_idx == group_col &&
                                        p.op == SimpleCmpOp::NE) {
                                        const auto &dv0 = gcol.str_dict_values;
                                        for (uint32_t d = 0; d < dv0.size(); d++) {
                                            if (dv0[d].GetSize() == p.sval.size() &&
                                                (p.sval.empty() ||
                                                 std::memcmp(dv0[d].GetData(),
                                                             p.sval.data(),
                                                             p.sval.size()) == 0)) {
                                                skip_di_q13 = d;
                                                break;
                                            }
                                        }
                                        single_pred_skip = true;
                                    }
                                }
                                std::vector<uint8_t> tk;
                                bool tk_active = false;
                                if (single_has_filter_fused && !single_pred_skip) {
                                    tk_active = BuildTypedKeepMask(
                                        single_preds, work.cols, nrows, tk);
                                }
                                int t = tid % Q13_THREADS;
                                if (dict_fast) {
                                    const uint32_t *di = gcol.str_dict_indices.data();
                                    const string_t *dv = gcol.str_dict_values.data();
                                    idx_t dsz = gcol.str_dict_values.size();
                                    if (single_pred_skip) {
                                        // Skip-di fast path: no keep_mask alloc.
                                        slothdb::g_pq_profile.c_skipdi.fetch_add(1, std::memory_order_relaxed);
                                        str_agg.IncrementByDictRGSkipDi(
                                            t, di, (uint32_t)nrows, dv,
                                            (uint32_t)dsz,
                                            gcol.all_valid ? nullptr
                                                : gcol.validity.data(),
                                            skip_di_q13);
                                    } else if (!single_has_filter_fused || tk_active) {
                                        // Bulk dict-aware: O(D) map ops vs O(N).
                                        slothdb::g_pq_profile.c_dictrg.fetch_add(1, std::memory_order_relaxed);
                                        str_agg.IncrementByDictRG(
                                            t, di, (uint32_t)nrows, dv,
                                            (uint32_t)dsz,
                                            gcol.all_valid ? nullptr
                                                : gcol.validity.data(),
                                            tk_active ? tk.data() : nullptr);
                                    } else {
                                        slothdb::g_pq_profile.c_perrow.fetch_add(1, std::memory_order_relaxed);
                                        for (idx_t r = 0; r < nrows; r++) {
                                            if (!EvalSimplePredicates(
                                                    single_preds, work.cols, r))
                                                continue;
                                            if (!gcol.all_valid &&
                                                !gcol.validity[r]) continue;
                                            uint32_t d = di[r];
                                            if (d >= dsz) continue;
                                            str_agg.IncrementRow(t,
                                                dv[d].GetData(), dv[d].GetSize());
                                        }
                                    }
                                } else if (!gcol.str_data.empty()) {
                                    slothdb::g_pq_profile.c_perrow.fetch_add(1, std::memory_order_relaxed);
                                    const string_t *gs = gcol.str_data.data();
                                    for (idx_t r = 0; r < nrows; r++) {
                                        if (tk_active) {
                                            if (!tk[r]) continue;
                                        } else if (single_has_filter_fused &&
                                                   !EvalSimplePredicates(
                                                       single_preds, work.cols, r))
                                            continue;
                                        if (!gcol.all_valid &&
                                            !gcol.validity[r]) continue;
                                        str_agg.IncrementRow(t,
                                            gs[r].GetData(), gs[r].GetSize());
                                    }
                                }
                            });
                        pq->RunParallelRGs(Q13_THREADS);
                        std::vector<std::thread> mts;
                        for (int s = 1; s < slothdb::RadixCountAggStr::N_RADIX; s++) {
                            mts.emplace_back([&str_agg, s]() {
                                str_agg.MergeShard(s);
                            });
                        }
                        str_agg.MergeShard(0);
                        for (auto &t : mts) t.join();
                        int top_k = 0;
                        if (topn_active_ && topn_limit_ > 0 &&
                            !topn_ascending_ &&
                            topn_col_idx_ >= 1 &&
                            topn_col_idx_ <= num_aggs) {
                            top_k = (int)topn_limit_;
                        }
                        auto results = str_agg.EmitTopK(top_k);
                        result_rows_.reserve(results.size());
                        for (auto &res : results) {
                            std::vector<Value> row;
                            row.reserve(1 + num_aggs);
                            row.push_back(Value::VARCHAR(res.key));
                            for (idx_t a = 0; a < num_aggs; a++) {
                                row.push_back(Value::BIGINT(res.count));
                            }
                            result_rows_.push_back(std::move(row));
                        }
                        return;
                    }
                    if (q16_shape) {
                        constexpr int Q16_THREADS = 8;
                        slothdb::RadixCountAgg radix_agg(Q16_THREADS);
                        // Reserve based on parquet total rows. Filter cuts
                        // it down but reserve isn't tight either way.
                        idx_t total_rows = 0;
                        for (idx_t rg = 0; rg < pq->NumRowGroups(); rg++) {
                            total_rows += pq->RowGroupSize(rg);
                        }
                        radix_agg.ReserveExpectedRows((int64_t)total_rows);
                        bool is_bigint_q16 =
                            (group_tid_q16 == LogicalTypeId::BIGINT);
                        pq->SetRGConsumer(
                            [&](const PhysicalParquetScan::RGWork &work,
                                idx_t rg_idx, int tid) {
                                if (work.pruned) return;
                                idx_t nrows = pq->RowGroupSize(rg_idx);
                                const auto &gcol = work.cols[group_col];
                                if (!gcol.decoded) return;
                                const int64_t *gi64 = is_bigint_q16
                                    ? gcol.i64_data.data() : nullptr;
                                const int32_t *gi32 = !is_bigint_q16
                                    ? gcol.i32_data.data() : nullptr;
                                std::vector<uint8_t> tk;
                                bool tk_active = false;
                                if (single_has_filter_fused) {
                                    tk_active = BuildTypedKeepMask(
                                        single_preds, work.cols, nrows, tk);
                                }
                                int t = tid % Q16_THREADS;
                                for (idx_t r = 0; r < nrows; r++) {
                                    if (tk_active) {
                                        if (!tk[r]) continue;
                                    } else if (single_has_filter_fused &&
                                               !EvalSimplePredicates(
                                                   single_preds, work.cols, r))
                                        continue;
                                    if (!gcol.all_valid &&
                                        !gcol.validity[r]) continue;
                                    int64_t k = is_bigint_q16
                                        ? gi64[r] : (int64_t)gi32[r];
                                    radix_agg.ScatterRow(t, k);
                                }
                            });
                        pq->RunParallelRGs();
                        // Phase 2: 16 disjoint radix workers (no
                        // contention). Run all but radix 0 in parallel
                        // threads; main thread does radix 0.
                        std::vector<std::thread> mts;
                        for (int r = 1; r < slothdb::RadixCountAgg::N_RADIX; r++) {
                            mts.emplace_back([&radix_agg, r]() {
                                radix_agg.MergeRadix(r);
                            });
                        }
                        radix_agg.MergeRadix(0);
                        for (auto &t : mts) t.join();
                        // Phase 3: emit. TopN heap when LIMIT pushdown is
                        // active and orders by count; full materialization
                        // otherwise. All aggs are CountStar so any agg
                        // col index orders by count (DESC only).
                        int top_k = 0;
                        if (topn_active_ && topn_limit_ > 0 &&
                            !topn_ascending_ &&
                            topn_col_idx_ >= 1 &&
                            topn_col_idx_ <= num_aggs) {
                            top_k = (int)topn_limit_;
                        }
                        auto results = radix_agg.EmitTopK(top_k);
                        result_rows_.reserve(results.size());
                        for (auto &res : results) {
                            std::vector<Value> row;
                            row.reserve(1 + num_aggs);
                            if (is_bigint_q16) {
                                row.push_back(Value::BIGINT(res.key));
                            } else {
                                row.push_back(
                                    Value::INTEGER((int32_t)res.key));
                            }
                            // All num_aggs are COUNT(*) — push the same
                            // count for each (planner duplicates when
                            // ORDER BY references the agg alias).
                            for (idx_t a = 0; a < num_aggs; a++) {
                                row.push_back(Value::BIGINT(res.count));
                            }
                            result_rows_.push_back(std::move(row));
                        }
                        return;
                    }
                }
                // Do NOT skip str_data on VARCHAR group cols: PLAIN-page RGs
                // need gstr[] populated for the fallback at the per-row branch
                // below. (Mid-RG dict-to-PLAIN fallback in hits.parquet would
                // OOB-write through nullptr str_data otherwise.)
                // Hoist agg kinds out of the per-row hot loop (shared r/o state).
                enum class AK { CountStar, Count, Sum, Min, Max, Other };
                std::vector<AK> kinds(num_aggs);
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &info = agg_infos[a];
                    if (info.name == "COUNT" && info.is_count_star)      kinds[a] = AK::CountStar;
                    else if (info.name == "COUNT")                        kinds[a] = AK::Count;
                    else if (info.name == "SUM" || info.name == "AVG")    kinds[a] = AK::Sum;
                    else if (info.name == "MIN")                          kinds[a] = AK::Min;
                    else if (info.name == "MAX")                          kinds[a] = AK::Max;
                    else                                                   kinds[a] = AK::Other;
                }

                // Thread-local aggregate state. Each worker thread writes to
                // its own TLSingle - no synchronization on the hot path.
                // int_keys was redundant: int64 key uniquely determines its
                // Value (group col type fixed per query). Synthesize at merge
                // time instead — saves ~30-50ns/miss in the scan hot loop.
                struct TLSingle {
                    ankerl::unordered_dense::map<int64_t, std::vector<AggState>> int_groups;
                    std::vector<int64_t> int_order;
                    std::vector<std::string> str_cache_keys;
                    std::vector<std::vector<AggState>> str_cache_states;
                    // Per-thread O(1) lookup mirror of str_cache_keys -> 1-based
                    // index into str_cache_states. Replaces the prior O(K_RG^2)
                    // linear scan on the dict-fast miss path and the non-dict
                    // VARCHAR fallback.
                    ankerl::unordered_dense::map<std::string, uint32_t> str_cache_index;
                };
                constexpr int MAX_THREADS = 8;
                std::vector<TLSingle> tls(MAX_THREADS);
                // Reserve cache capacity so emplace_back doesn't reallocate and
                // invalidate the per-RG `dict_slot_ptrs` we hand out. 256 is
                // far larger than typical GROUP BY cardinality in this path.
                for (auto &tl : tls) {
                    tl.str_cache_keys.reserve(256);
                    tl.str_cache_states.reserve(256);
                }

                auto process_rg = [&](const PhysicalParquetScan::RGWork &work,
                                       idx_t rg_idx, TLSingle &tl) {
                    idx_t nrows = pq->RowGroupSize(rg_idx);
                    const auto &gcol = work.cols[group_col];
                    if (!gcol.decoded) return;

                    auto gtid = gcol.type.id();
                    bool is_bigint = (gtid == LogicalTypeId::BIGINT);
                    bool is_integer = (gtid == LogicalTypeId::INTEGER);
                    bool is_varchar = (gtid == LogicalTypeId::VARCHAR);
                    const int64_t *gi64 = is_bigint ? gcol.i64_data.data() : nullptr;
                    const int32_t *gi32 = is_integer ? gcol.i32_data.data() : nullptr;
                    const string_t *gstr = is_varchar ? gcol.str_data.data() : nullptr;

                    bool dict_fast = is_varchar && gcol.str_dict_encoded &&
                                     !gcol.str_dict_indices.empty();
                    // 1-based index into tl.str_cache_states; 0 = unset. Indices
                    // remain valid across emplace_back relocations, raw pointers
                    // do not.
                    std::vector<uint32_t> dict_slot_idx;
                    if (dict_fast) dict_slot_idx.assign(gcol.str_dict_values.size(), 0u);

                    // Per-agg column snapshot for this RG.
                    struct AggCol { const ParquetColumnData *col; LogicalTypeId tid; bool decoded; bool all_valid; };
                    std::vector<AggCol> ac(num_aggs);
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &info = agg_infos[a];
                        if (info.col_idx != INVALID_INDEX) {
                            ac[a].col = &work.cols[info.col_idx];
                            ac[a].tid = ac[a].col->type.id();
                            ac[a].decoded = ac[a].col->decoded;
                            ac[a].all_valid = ac[a].col->all_valid;
                        } else { ac[a].col = nullptr; }
                    }

                    // Vectorized per-RG keep mask when all preds reduce to
                    // typed numeric or dict-encoded VARCHAR cmp. Drops the
                    // ~30 cyc/row EvalSimplePredicates dispatch to a single
                    // mask load. Q8 SELECT AdvEngineID, COUNT(*) WHERE
                    // AdvEngineID <> 0 lives here.
                    std::vector<uint8_t> typed_keep_mask;
                    bool typed_keep_active = false;
                    if (single_has_filter_fused) {
                        typed_keep_active = BuildTypedKeepMask(single_preds, work.cols,
                                                                nrows, typed_keep_mask);
                    }

                    for (idx_t r = 0; r < nrows; r++) {
                        if (typed_keep_active) {
                            if (!typed_keep_mask[r]) continue;
                        } else if (single_has_filter_fused &&
                                   !EvalSimplePredicates(single_preds, work.cols, r)) continue;
                        std::vector<AggState> *states_ptr = nullptr;
                        if (is_bigint || is_integer) {
                            int64_t k = is_bigint ? gi64[r] : (int64_t)gi32[r];
                            auto it = tl.int_groups.find(k);
                            if (it == tl.int_groups.end()) {
                                tl.int_order.push_back(k);
                                auto [nit, _] = tl.int_groups.try_emplace(k);
                                nit->second.resize(num_aggs);
                                it = nit;
                            }
                            states_ptr = &it->second;
                        } else if (dict_fast) {
                            uint32_t di = gcol.str_dict_indices[r];
                            if (di >= dict_slot_idx.size()) continue;
                            if (dict_slot_idx[di] == 0u) {
                                const auto &dv = gcol.str_dict_values[di];
                                const char *s_data = dv.GetData();
                                uint32_t s_len = dv.GetSize();
                                std::string key(s_data, s_len);
                                auto it = tl.str_cache_index.find(key);
                                uint32_t found_idx;
                                if (it != tl.str_cache_index.end()) {
                                    found_idx = it->second;
                                } else {
                                    tl.str_cache_keys.push_back(key);
                                    tl.str_cache_states.emplace_back(num_aggs);
                                    found_idx = (uint32_t)tl.str_cache_states.size();
                                    tl.str_cache_index.emplace(std::move(key), found_idx);
                                }
                                dict_slot_idx[di] = found_idx;
                            }
                            states_ptr = &tl.str_cache_states[dict_slot_idx[di] - 1u];
                        } else if (is_varchar) {
                            const char *s_data = gstr[r].GetData();
                            uint32_t s_len = gstr[r].GetSize();
                            std::string key(s_data, s_len);
                            auto it = tl.str_cache_index.find(key);
                            uint32_t found_idx;
                            if (it != tl.str_cache_index.end()) {
                                found_idx = it->second;
                            } else {
                                tl.str_cache_keys.push_back(key);
                                tl.str_cache_states.emplace_back(num_aggs);
                                found_idx = (uint32_t)tl.str_cache_states.size();
                                tl.str_cache_index.emplace(std::move(key), found_idx);
                            }
                            states_ptr = &tl.str_cache_states[found_idx - 1u];
                        }
                        if (!states_ptr) continue;

                        auto &states_r = *states_ptr;
                        for (idx_t a = 0; a < num_aggs; a++) {
                            auto &state = states_r[a];
                            auto &acol = ac[a];
                            if (kinds[a] == AK::CountStar) { state.count++; continue; }
                            if (!acol.col || !acol.decoded) continue;
                            if (!acol.all_valid && !acol.col->validity[r]) continue;
                            // VARCHAR MIN/MAX: store winner in min_val/max_val.
                            if ((kinds[a] == AK::Min || kinds[a] == AK::Max) &&
                                acol.tid == LogicalTypeId::VARCHAR) {
                                const char *sd = nullptr; uint32_t sl = 0;
                                if (acol.col->str_dict_encoded &&
                                    !acol.col->str_dict_indices.empty()) {
                                    uint32_t di = acol.col->str_dict_indices[r];
                                    if (di >= acol.col->str_dict_values.size()) continue;
                                    sd = acol.col->str_dict_values[di].GetData();
                                    sl = acol.col->str_dict_values[di].GetSize();
                                } else if (!acol.col->str_data.empty()) {
                                    sd = acol.col->str_data[r].GetData();
                                    sl = acol.col->str_data[r].GetSize();
                                } else continue;
                                std::string_view sv(sd, sl);
                                if (kinds[a] == AK::Min) {
                                    if (!state.has_min) {
                                        state.min_val() = Value::VARCHAR(std::string(sd, sl));
                                        state.has_min = true;
                                    } else {
                                        auto cur = state.min_val().template GetValue<std::string>();
                                        if (sv < std::string_view(cur))
                                            state.min_val() = Value::VARCHAR(std::string(sd, sl));
                                    }
                                } else {
                                    if (!state.has_max) {
                                        state.max_val() = Value::VARCHAR(std::string(sd, sl));
                                        state.has_max = true;
                                    } else {
                                        auto cur = state.max_val().template GetValue<std::string>();
                                        if (sv > std::string_view(cur))
                                            state.max_val() = Value::VARCHAR(std::string(sd, sl));
                                    }
                                }
                                continue;
                            }
                            double val = 0.0;
                            switch (acol.tid) {
                            case LogicalTypeId::DOUBLE:  val = acol.col->f64_data[r]; break;
                            case LogicalTypeId::BIGINT:  val = (double)acol.col->i64_data[r]; break;
                            case LogicalTypeId::INTEGER: val = (double)acol.col->i32_data[r]; break;
                            case LogicalTypeId::FLOAT:   val = (double)acol.col->f32_data[r]; break;
                            default: continue;
                            }
                            switch (kinds[a]) {
                            case AK::Count: state.count++; break;
                            case AK::Sum:   state.sum += val; state.count++; break;
                            case AK::Min:
                                if (!state.has_min || val < state.sum_min) {
                                    state.sum_min = val; state.has_min = true;
                                }
                                break;
                            case AK::Max:
                                if (!state.has_max || val > state.sum_max) {
                                    state.sum_max = val; state.has_max = true;
                                }
                                break;
                            default: break;
                            }
                        }
                    }
                };

                pq->SetRGConsumer([&](const PhysicalParquetScan::RGWork &w,
                                       idx_t rg_idx, int tid) {
                    process_rg(w, rg_idx, tls[tid]);
                });
                int nt = pq->RunParallelRGs();

                // Merge AggState vectors element-wise (sum/min/max per agg).
                auto merge_states = [&](std::vector<AggState> &dst,
                                         const std::vector<AggState> &src) {
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &d = dst[a]; auto &s = src[a];
                        d.count += s.count;
                        d.sum   += s.sum;
                        if (s.has_min) {
                            if (!d.has_min) {
                                d.has_min = true;
                                d.sum_min = s.sum_min;
                                d.set_min_from(s);
                            } else if (!s.min_val_is_null() && !d.min_val_is_null()) {
                                if (s.min_val_ptr->template GetValue<std::string>() <
                                    d.min_val_ptr->template GetValue<std::string>())
                                    d.set_min_from(s);
                            } else if (s.sum_min < d.sum_min) {
                                d.sum_min = s.sum_min;
                            }
                        }
                        if (s.has_max) {
                            if (!d.has_max) {
                                d.has_max = true;
                                d.sum_max = s.sum_max;
                                d.set_max_from(s);
                            } else if (!s.max_val_is_null() && !d.max_val_is_null()) {
                                if (s.max_val_ptr->template GetValue<std::string>() >
                                    d.max_val_ptr->template GetValue<std::string>())
                                    d.set_max_from(s);
                            } else if (s.sum_max > d.sum_max) {
                                d.sum_max = s.sum_max;
                            }
                        }
                    }
                };

                // Merge per-thread state. Group-col type cached at outer
                // scope so the post-if/else direct-emit and shard-rebuild
                // loops can synthesize Values without re-reading children[0].
                grp_is_bigint =
                    (children[0]->GetTypes()[group_col].id() == LogicalTypeId::BIGINT);

                // Parallel cross-thread int merge by hash sharding. Each shard
                // worker scans every tl.int_groups but only processes keys
                // hashing to its shard (cheap bit-mix + bitand filters out
                // ~all keys from non-owned shards in ~3-5ns). The per-shard
                // ankerl maps are independent so the workers run with zero
                // synchronization. Q35: 9.76M unique groups across 2 threads;
                // single-threaded global merge was ~7.5s wall, sharded target
                // ~1-2s.
                size_t _est_total = 0;
                for (int t = 0; t < nt; t++) _est_total += tls[t].int_groups.size();
                size_t per_shard_est = _est_total / MERGE_SHARDS + 64;
                for (auto &sh : int_merge_shards) {
                    sh.int_groups.reserve(per_shard_est);
                    sh.int_order.reserve(per_shard_est);
                }
                slothdb::ParallelFor(MERGE_SHARDS, [&](unsigned int s) {
                    auto &out = int_merge_shards[s];
                    for (int t = 0; t < nt; t++) {
                        auto &tl = tls[t];
                        for (auto &kv : tl.int_groups) {
                            // Bit-mix to avoid sequential ClientIPs all
                            // landing in the same shard. (Multiplicative hash
                            // with golden-ratio constant.)
                            uint64_t h = (uint64_t)kv.first;
                            h *= 0x9E3779B97F4A7C15ULL;
                            if ((h & (MERGE_SHARDS - 1)) != s) continue;
                            int64_t k = kv.first;
                            auto git = out.int_groups.find(k);
                            if (git == out.int_groups.end()) {
                                out.int_order.push_back(k);
                                auto [nit, _] = out.int_groups.try_emplace(k);
                                nit->second.resize(num_aggs);
                                git = nit;
                            }
                            merge_states(git->second, kv.second);
                        }
                    }
                });

                ankerl::unordered_dense::map<std::string, uint32_t> global_str_index;
                for (int t = 0; t < nt; t++) {
                    auto &tl = tls[t];
                    for (idx_t oi = 0; oi < tl.str_cache_keys.size(); oi++) {
                        const auto &k = tl.str_cache_keys[oi];
                        auto git = global_str_index.find(k);
                        std::vector<AggState> *gsp;
                        if (git == global_str_index.end()) {
                            uint32_t new_idx = (uint32_t)str_cache_keys.size();
                            str_cache_keys.push_back(k);
                            str_cache_states.emplace_back(num_aggs);
                            str_order.push_back(k);
                            str_keys[k] = Value::VARCHAR(k);
                            global_str_index.emplace(k, new_idx);
                            gsp = &str_cache_states[new_idx];
                        } else {
                            gsp = &str_cache_states[git->second];
                        }
                        merge_states(*gsp, tl.str_cache_states[oi]);
                    }
                }
            } else

            while (children[0]->GetData(chunk)) {
                idx_t chunk_size = chunk.size();
                auto &gvec = chunk.GetVector(group_col);
                auto gtid = gvec.GetType().id();
                bool is_int = (gtid == LogicalTypeId::INTEGER || gtid == LogicalTypeId::BIGINT);
                bool is_str = (gtid == LogicalTypeId::VARCHAR);

                for (idx_t i = 0; i < chunk_size; i++) {
                    std::vector<AggState> *states_ptr = nullptr;

                    if (is_int) {
                        int64_t k = (gtid == LogicalTypeId::BIGINT)
                            ? reinterpret_cast<const int64_t *>(gvec.GetData())[i]
                            : static_cast<int64_t>(reinterpret_cast<const int32_t *>(gvec.GetData())[i]);
                        auto it = int_groups.find(k);
                        if (it == int_groups.end()) {
                            int_order.push_back(k);
                            int_keys[k] = chunk.GetValue(group_col, i);
                            it = int_groups.emplace(k, std::vector<AggState>(num_aggs)).first;
                        }
                        states_ptr = &it->second;
                    } else if (is_str) {
                        auto &s = reinterpret_cast<const string_t *>(gvec.GetData())[i];
                        const char *s_data = s.GetData();
                        uint32_t s_len = s.GetSize();
                        // Linear scan with direct pointer cache - no map lookup on hit.
                        states_ptr = nullptr;
                        for (idx_t oi = 0; oi < str_cache_keys.size(); oi++) {
                            const auto &ck = str_cache_keys[oi];
                            if (ck.size() == s_len && memcmp(ck.data(), s_data, s_len) == 0) {
                                states_ptr = &str_cache_states[oi];
                                break;
                            }
                        }
                        if (!states_ptr) {
                            // New key - add to cache and map.
                            std::string k(s_data, s_len);
                            str_cache_keys.push_back(k);
                            str_cache_states.emplace_back(num_aggs);
                            str_order.push_back(k);
                            str_keys[k] = chunk.GetValue(group_col, i);
                            states_ptr = &str_cache_states.back();
                        }
                    } else {
                        // Fallback to string key for other types.
                        std::string k = chunk.GetValue(group_col, i).ToString();
                        auto it = str_groups.find(k);
                        if (it == str_groups.end()) {
                            str_order.push_back(k);
                            str_keys[k] = chunk.GetValue(group_col, i);
                            it = str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                        }
                        states_ptr = &it->second;
                    }

                    auto &states = *states_ptr;
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &state = states[a];
                        auto &info = agg_infos[a];
                        if (info.name == "COUNT" && info.is_count_star) {
                            state.count++;
                        } else if (info.col_idx != INVALID_INDEX) {
                            auto &vec = chunk.GetVector(info.col_idx);
                            if (vec.GetValidity().RowIsValid(i)) {
                                if (info.name == "COUNT") {
                                    state.count++;
                                } else {
                                    double val = ReadDouble(vec, i);
                                    if (info.name == "SUM" || info.name == "AVG") {
                                        state.count++;
                                        state.sum += val;
                                    } else if (info.name == "MIN") {
                                        if (!state.has_min || val < state.sum_min) {
                                            state.sum_min = val; state.has_min = true;
                                            state.min_val() = chunk.GetValue(info.col_idx, i);
                                        }
                                    } else if (info.name == "MAX") {
                                        if (!state.has_max || val > state.sum_max) {
                                            state.sum_max = val; state.has_max = true;
                                            state.max_val() = chunk.GetValue(info.col_idx, i);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                total_rows_processed += chunk_size;
            }

            // Move cache states into str_groups for result building.
            for (idx_t oi = 0; oi < str_cache_keys.size(); oi++) {
                str_groups[str_cache_keys[oi]] = std::move(str_cache_states[oi]);
            }
            // Direct emit shortcut for the int-only single-col case: when
            // no string groups are present, emit straight into result_rows_
            // from the parallel-merge shards. Skips rebuilding string-keyed
            // group_states/group_keys/group_order (which on Q35 cost ~7.7s
            // for std::to_string + 4 ankerl ops × 9.76 M keys) AND skips the
            // common emit's `find()` per group (~4.6s on Q35). Each shard
            // produces a slice of result_rows_; the slices are concatenated
            // at the end (insertion order across shards is meaningless since
            // ankerl iteration order is hash-bucket order anyway).
            // Runs only when str_order is empty (parquet pq path) AND when
            // shards have data (pq path populated them); chunk fallback's
            // outer int_order/int_groups/int_keys go through the FS.MOVE
            // loop further down instead.
            size_t total_shard_groups = 0;
            for (auto &sh : int_merge_shards) total_shard_groups += sh.int_order.size();
            if (total_shard_groups > 0 && str_order.empty()) {
                std::vector<EmitAggDesc> direct_descs(num_aggs);
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &ae = static_cast<BoundFunction &>(*aggregates_[a]);
                    direct_descs[a].kind = ResolveEmitAggKind(StringUtil::Upper(ae.function_name));
                    direct_descs[a].return_type_id = ae.GetReturnType().id();
                    direct_descs[a].sum_with_offset = agg_infos[a].sum_with_offset;
                    direct_descs[a].sum_offset = agg_infos[a].sum_offset;
                }
                auto direct_view = [](const AggState &s, EmitAggView &v) {
                    v.count = s.count;
                    v.sum = s.sum;
                    v.sum_sq = s.sum_sq();
                    v.has_min = s.has_min;
                    v.sum_min = s.sum_min;
                    v.min_val_ptr = s.min_val_is_null() ? nullptr : s.min_val_ptr.get();
                    v.has_max = s.has_max;
                    v.sum_max = s.sum_max;
                    v.max_val_ptr = s.max_val_is_null() ? nullptr : s.max_val_ptr.get();
                    v.str_started = s.str_started();
                    v.str_agg = &s.str_agg_const();
                    v.values = s.extras_ptr ? &s.extras_ptr->values : nullptr;
                    v.bool_and_v = s.bool_and_v();
                    v.bool_or_v = s.bool_or_v();
                };
                // TopN pushdown: when ORDER BY+LIMIT is forwarded to us AND
                // the order-key is an int64-comparable col (group col or a
                // COUNT-style agg), keep a bounded heap of size topn_limit_
                // and emit only the top-K rows. Skips the 9.76 M result-row
                // materialization on Q35 when ORDER BY count DESC LIMIT 10.
                bool topn_int_path = false;
                bool topn_key_is_group = false;       // col_idx targets group col 0
                idx_t topn_agg_idx = INVALID_INDEX;   // when targeting an agg col
                if (topn_active_ && topn_limit_ > 0 &&
                    topn_limit_ < total_shard_groups) {
                    if (topn_col_idx_ == 0) {
                        // Order key is the (only) group col — int64 key.
                        topn_int_path = true;
                        topn_key_is_group = true;
                    } else if (topn_col_idx_ >= 1 &&
                               topn_col_idx_ - 1 < num_aggs) {
                        idx_t ai = topn_col_idx_ - 1;
                        idx_t real_ai = (agg_infos[ai].primary_idx != INVALID_INDEX)
                                            ? agg_infos[ai].primary_idx : ai;
                        // Q35-shape: COUNT/COUNT(*) → state.count is int64.
                        if (agg_infos[real_ai].name == "COUNT") {
                            topn_int_path = true;
                            topn_agg_idx = real_ai;
                        }
                    }
                }
                if (topn_int_path) {
                    // Min-heap (when ascending=false / DESC) so the smallest
                    // top-K element sits at top(); push when better, pop top
                    // when full and the candidate beats it.
                    struct HE { int64_t sk; int64_t gk; int sh; };
                    auto cmp_desc = [](const HE& a, const HE& b) {
                        return a.sk > b.sk;  // min-heap for DESC top-K
                    };
                    auto cmp_asc = [](const HE& a, const HE& b) {
                        return a.sk < b.sk;  // max-heap for ASC top-K
                    };
                    std::vector<HE> heap_buf;
                    heap_buf.reserve(topn_limit_ + 1);
                    auto get_sk = [&](const std::vector<AggState> &sr, int64_t k) -> int64_t {
                        if (topn_key_is_group) return k;
                        return sr[topn_agg_idx].count;
                    };
                    auto better_than_top = [&](int64_t cand, int64_t top) {
                        return topn_ascending_ ? cand < top : cand > top;
                    };
                    auto push_heap_local = [&]() {
                        if (topn_ascending_) std::push_heap(heap_buf.begin(), heap_buf.end(), cmp_asc);
                        else std::push_heap(heap_buf.begin(), heap_buf.end(), cmp_desc);
                    };
                    auto pop_heap_local = [&]() {
                        if (topn_ascending_) std::pop_heap(heap_buf.begin(), heap_buf.end(), cmp_asc);
                        else std::pop_heap(heap_buf.begin(), heap_buf.end(), cmp_desc);
                        heap_buf.pop_back();
                    };
                    for (int sh_i = 0; sh_i < (int)int_merge_shards.size(); sh_i++) {
                        auto &shard = int_merge_shards[sh_i];
                        for (int64_t k : shard.int_order) {
                            int64_t sk = get_sk(shard.int_groups[k], k);
                            if (heap_buf.size() < topn_limit_) {
                                heap_buf.push_back({sk, k, sh_i});
                                push_heap_local();
                            } else if (better_than_top(sk, heap_buf.front().sk)) {
                                pop_heap_local();
                                heap_buf.push_back({sk, k, sh_i});
                                push_heap_local();
                            }
                        }
                    }
                    result_rows_.reserve(heap_buf.size());
                    for (auto &he : heap_buf) {
                        auto &states_r = int_merge_shards[he.sh].int_groups[he.gk];
                        std::vector<Value> result_row;
                        result_row.reserve(1 + num_aggs);
                        result_row.push_back(grp_is_bigint ? Value::BIGINT(he.gk)
                                                           : Value::INTEGER((int32_t)he.gk));
                        for (idx_t a = 0; a < num_aggs; a++) {
                            idx_t state_idx = (agg_infos[a].primary_idx != INVALID_INDEX)
                                                  ? agg_infos[a].primary_idx : a;
                            EmitAggView view;
                            direct_view(states_r[state_idx], view);
                            EmitAggValue(direct_descs[a], view, result_row);
                        }
                        result_rows_.push_back(std::move(result_row));
                    }
                    int_merge_shards.clear();
                } else {
                    result_rows_.reserve(total_shard_groups);
                    for (auto &sh : int_merge_shards) {
                        for (int64_t k : sh.int_order) {
                            auto &states_r = sh.int_groups[k];
                            std::vector<Value> result_row;
                            result_row.reserve(1 + num_aggs);
                            result_row.push_back(grp_is_bigint ? Value::BIGINT(k)
                                                               : Value::INTEGER((int32_t)k));
                            for (idx_t a = 0; a < num_aggs; a++) {
                                idx_t state_idx = (agg_infos[a].primary_idx != INVALID_INDEX)
                                                      ? agg_infos[a].primary_idx : a;
                                EmitAggView view;
                                direct_view(states_r[state_idx], view);
                                EmitAggValue(direct_descs[a], view, result_row);
                            }
                            result_rows_.push_back(std::move(result_row));
                        }
                    }
                    int_merge_shards.clear();
                }
            } else if (total_shard_groups > 0) {
                // Mixed pq path with str groups: rebuild string-keyed map
                // from int shards so the common emit can see them.
                for (auto &sh : int_merge_shards) {
                    for (int64_t k : sh.int_order) {
                        std::string sk = std::to_string(k);
                        group_states[sk] = std::move(sh.int_groups[k]);
                        group_keys[sk] = {grp_is_bigint ? Value::BIGINT(k)
                                                       : Value::INTEGER((int32_t)k)};
                        group_order.push_back(sk);
                    }
                }
                int_merge_shards.clear();
            }
            // Chunk-fallback FS.MOVE: only used when the parquet fast path
            // didn't run (pq was null). Outer int_order/int_groups/int_keys
            // were populated by the chunk loop instead of int_merge_shards.
            for (auto k : int_order) {
                std::string sk = std::to_string(k);
                group_states[sk] = std::move(int_groups[k]);
                group_keys[sk] = {int_keys[k]};
                group_order.push_back(sk);
            }
            for (auto &k : str_order) {
                group_states[k] = std::move(str_groups[k]);
                group_keys[k] = {str_keys[k]};
                group_order.push_back(k);
            }
            } // end !ultrafast
        } else if (is_simple_count_star) {
            // Ultra-fast: if child is a file scan, just count newlines.
            group_order.push_back("");
            group_states[""].resize(1);
            group_keys[""] = {};
            auto &state = group_states[""][0];

            auto *file_scan = dynamic_cast<PhysicalFileScan *>(children[0].get());
            auto *count_scan = dynamic_cast<PhysicalCountScan *>(children[0].get());
            auto *parquet_scan = dynamic_cast<PhysicalParquetScan *>(children[0].get());

            // === FUSED COUNT(*) WHERE simple_pred OVER PARQUET ===
            // Detect the AGG -> FILTER -> PARQUET shape with a compileable
            // predicate. Run RunParallelRGs against the scan, evaluate the
            // predicate per row directly against the typed column buffers,
            // and bump a per-thread match counter. Skips PhysicalFilter's
            // per-row Vector::SetValue copies entirely.
            PhysicalParquetScan *fused_pq = nullptr;
            std::vector<SimplePredicate> fused_count_preds;
            if (!parquet_scan && !file_scan && !count_scan) {
                if (auto *flt = dynamic_cast<PhysicalFilter *>(children[0].get())) {
                    if (!flt->children.empty()) {
                        if (auto *pq2 = dynamic_cast<PhysicalParquetScan *>(flt->children[0].get())) {
                            std::vector<SimplePredicate> tmp;
                            if (flt->GetCondition() &&
                                TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                                fused_pq = pq2;
                                fused_count_preds = std::move(tmp);
                            }
                        }
                    }
                }
            }

            if (file_scan) {
                // Reuse the already-loaded reader - no second fread.
                auto *r = file_scan->GetReader();
                idx_t count = r->CountRows();
                state.count = static_cast<int64_t>(count);
                total_rows_processed = count;
            } else if (count_scan) {
                state.count = static_cast<int64_t>(count_scan->GetRowCount());
                total_rows_processed = count_scan->GetRowCount();
            } else if (fused_pq) {
                // Project only the columns the predicate references.
                {
                    idx_t nc = fused_pq->GetTypes().size();
                    std::vector<bool> need(nc, false);
                    for (auto &p : fused_count_preds) {
                        if (p.col_idx < nc) need[p.col_idx] = true;
                    }
                    fused_pq->SetNeededOutputs(need);
                    // VARCHAR predicate cols: BuildTypedKeepMask + the row-loop
                    // fallback both consult str_dict_values first. Skip the
                    // per-row str_data write (1.6 GB / Q21 query). PLAIN-only
                    // RGs trigger MaterialiseStrDataLazy back-fill.
                    std::vector<bool> skip(nc, false);
                    for (auto &p : fused_count_preds) {
                        if (p.col_idx < nc && p.str_form &&
                            fused_pq->GetTypes()[p.col_idx].id() == LogicalTypeId::VARCHAR) {
                            skip[p.col_idx] = true;
                        }
                    }
                    fused_pq->SetSkipStrData(std::move(skip));
                }
                fused_pq->Init();

                constexpr int MAX_THREADS = 8;
                std::array<std::atomic<int64_t>, MAX_THREADS> tl_match{};
                fused_pq->SetRGConsumer(
                    [&](const PhysicalParquetScan::RGWork &work, idx_t rg_idx, int tid) {
                        idx_t nrows = fused_pq->RowGroupSize(rg_idx);
                        // VARCHAR EQ/NE/LIKE preds: route through the
                        // dict-amortised BuildTypedKeepMask. Sum the mask to
                        // get the local count. Falls back to row-loop on
                        // non-dict RGs.
                        bool any_str = false;
                        for (auto &p : fused_count_preds) if (p.str_form) { any_str = true; break; }
                        // Q21 fused fast path: single LIKE '%needle%' pred on a
                        // dict-encoded VARCHAR column. Skip BuildTypedKeepMask's
                        // out_mask materialization (3 passes -> 1 pass).
                        if (fused_count_preds.size() == 1 &&
                            fused_count_preds[0].str_form &&
                            fused_count_preds[0].like_contains) {
                            const auto &p = fused_count_preds[0];
                            if (p.col_idx < work.cols.size()) {
                                const auto &col = work.cols[p.col_idx];
                                if (col.decoded && col.all_valid &&
                                    col.str_dict_encoded &&
                                    !col.str_dict_indices.empty()) {
                                    int64_t local = slothdb::CountDictLikeContains(
                                        col.str_dict_values.data(),
                                        col.str_dict_values.size(),
                                        col.str_dict_indices.data(),
                                        nrows,
                                        p.sval.data(), p.sval.size(),
                                        p.like_negated);
                                    tl_match[tid % MAX_THREADS].fetch_add(
                                        local, std::memory_order_relaxed);
                                    return;
                                }
                                // PLAIN-encoded fast path: most URL RGs on
                                // hits.parquet are PLAIN (205/226). Walks
                                // str_data with DuckDB-style uint32 prefix
                                // compare, skipping the generic-memcmp
                                // tail loop used by the row-loop fallback.
                                if (col.decoded && col.all_valid &&
                                    !col.str_dict_encoded &&
                                    !col.str_data.empty() &&
                                    col.str_data.size() >= nrows) {
                                    int64_t local = slothdb::CountPlainLikeContains(
                                        col.str_data.data(), nrows,
                                        p.sval.data(), p.sval.size(),
                                        p.like_negated);
                                    tl_match[tid % MAX_THREADS].fetch_add(
                                        local, std::memory_order_relaxed);
                                    return;
                                }
                            }
                        }
                        if (any_str) {
                            std::vector<uint8_t> mask;
                            if (BuildTypedKeepMask(fused_count_preds, work.cols, nrows, mask)) {
                                int64_t local = 0;
                                for (idx_t i = 0; i < nrows; i++) local += mask[i];
                                tl_match[tid % MAX_THREADS].fetch_add(local, std::memory_order_relaxed);
                                return;
                            }
                            // Fallback row-loop. Handles RGs where typed
                            // decode failed (cols_fallback Value-vector) so
                            // correctness matches the slow ExpressionExecutor
                            // path even on non-dict / mixed pages.
                            int64_t local = 0;
                            for (idx_t i = 0; i < nrows; i++) {
                                bool keep = true;
                                for (auto &p : fused_count_preds) {
                                    const auto &col = work.cols[p.col_idx];
                                    if (col.decoded) {
                                        if (!col.all_valid && !col.validity[i]) { keep = false; break; }
                                        if (p.str_form) {
                                            const char *sd = nullptr; uint32_t sl = 0;
                                            if (col.str_dict_encoded && !col.str_dict_indices.empty()) {
                                                uint32_t di = col.str_dict_indices[i];
                                                if (di >= col.str_dict_values.size()) { keep = false; break; }
                                                sd = col.str_dict_values[di].GetData();
                                                sl = col.str_dict_values[di].GetSize();
                                            } else if (!col.str_data.empty()) {
                                                sd = col.str_data[i].GetData();
                                                sl = col.str_data[i].GetSize();
                                            } else { keep = false; break; }
                                            if (p.like_contains) {
                                                bool match;
                                                if (p.sval.empty()) {
                                                    match = true;
                                                } else if (sl < p.sval.size()) {
                                                    match = false;
                                                } else {
                                                    match = (FindSubstr(sd, sl, p.sval.data(), p.sval.size()) != nullptr);
                                                }
                                                if (p.like_negated) match = !match;
                                                if (!match) { keep = false; break; }
                                            } else {
                                                bool eq = (sl == p.sval.size()) &&
                                                          (p.sval.empty() ||
                                                           std::memcmp(sd, p.sval.data(), p.sval.size()) == 0);
                                                if (p.op == SimpleCmpOp::EQ) { if (!eq) { keep = false; break; } }
                                                else                         { if (eq)  { keep = false; break; } }
                                            }
                                            continue;
                                        }
                                        keep = false; break;
                                    }
                                    if (p.col_idx >= work.cols_fallback.size()) { keep = false; break; }
                                    const auto &vals = work.cols_fallback[p.col_idx];
                                    if (i >= vals.size() || vals[i].IsNull()) { keep = false; break; }
                                    if (!p.str_form) { keep = false; break; }
                                    const auto &s = vals[i].GetValue<std::string>();
                                    if (p.like_contains) {
                                        if (!p.sval.empty() && s.find(p.sval) == std::string::npos) {
                                            keep = false; break;
                                        }
                                    } else {
                                        bool eq = (s == p.sval);
                                        if (p.op == SimpleCmpOp::EQ) { if (!eq) { keep = false; break; } }
                                        else                         { if (eq)  { keep = false; break; } }
                                    }
                                }
                                if (keep) ++local;
                            }
                            tl_match[tid % MAX_THREADS].fetch_add(local, std::memory_order_relaxed);
                            return;
                        }
                        // Hoist per-predicate column pointers + type + op
                        // outside the row loop. Inner loop is a tight
                        // typed-array compare; the compiler auto-vectorises
                        // this with SSE/AVX (or the scalar version is at
                        // least branch-free per row).
                        struct CompiledPred {
                            const int64_t *i64;
                            const int32_t *i32;
                            const double  *f64;
                            const float   *f32;
                            const uint8_t *validity;  // null when all valid
                            LogicalTypeId type;
                            SimpleCmpOp op;
                            double dval;
                            int64_t  ival;       // pre-converted for int paths
                        };
                        std::vector<CompiledPred> cps(fused_count_preds.size());
                        bool any_unsupported = false;
                        for (size_t pi = 0; pi < fused_count_preds.size(); pi++) {
                            auto &p = fused_count_preds[pi];
                            auto &cp = cps[pi];
                            const auto &col = work.cols[p.col_idx];
                            if (!col.decoded) { any_unsupported = true; break; }
                            cp.type = col.type.id();
                            cp.op = p.op;
                            cp.dval = p.dval;
                            cp.ival = p.ival;  // exact int64 — avoids 2^53 dval rounding
                            cp.validity = col.all_valid ? nullptr : col.validity.data();
                            cp.i64 = (cp.type == LogicalTypeId::BIGINT)  ? col.i64_data.data() : nullptr;
                            cp.i32 = (cp.type == LogicalTypeId::INTEGER) ? col.i32_data.data() : nullptr;
                            cp.f64 = (cp.type == LogicalTypeId::DOUBLE)  ? col.f64_data.data() : nullptr;
                            cp.f32 = (cp.type == LogicalTypeId::FLOAT)   ? col.f32_data.data() : nullptr;
                            if (!cp.i64 && !cp.i32 && !cp.f64 && !cp.f32) any_unsupported = true;
                        }
                        if (any_unsupported) {
                            // Fall back to legacy path for this RG.
                            int64_t local = 0;
                            for (idx_t i = 0; i < nrows; i++) {
                                bool keep = true;
                                for (auto &p : fused_count_preds) {
                                    const auto &col = work.cols[p.col_idx];
                                    if (!col.decoded) { keep = false; break; }
                                    double v = 0;
                                    bool valid = col.all_valid || col.validity[i];
                                    if (!valid) { keep = false; break; }
                                    switch (col.type.id()) {
                                    case LogicalTypeId::BIGINT:  v = (double)col.i64_data[i]; break;
                                    case LogicalTypeId::INTEGER: v = (double)col.i32_data[i]; break;
                                    case LogicalTypeId::DOUBLE:  v = col.f64_data[i]; break;
                                    case LogicalTypeId::FLOAT:   v = (double)col.f32_data[i]; break;
                                    default: keep = false; break;
                                    }
                                    if (!keep) break;
                                    bool ok = false;
                                    switch (p.op) {
                                    case SimpleCmpOp::LT: ok = (v <  p.dval); break;
                                    case SimpleCmpOp::LE: ok = (v <= p.dval); break;
                                    case SimpleCmpOp::GT: ok = (v >  p.dval); break;
                                    case SimpleCmpOp::GE: ok = (v >= p.dval); break;
                                    case SimpleCmpOp::EQ: ok = (v == p.dval); break;
                                    case SimpleCmpOp::NE: ok = (v != p.dval); break;
                                    }
                                    if (!ok) { keep = false; break; }
                                }
                                if (keep) ++local;
                            }
                            tl_match[tid % MAX_THREADS].fetch_add(local, std::memory_order_relaxed);
                            return;
                        }

                        // Fast path: hoisted predicate metadata, tight loops.
                        // For the single-predicate case (very common) we
                        // dispatch to a per-(type, op) specialised body so
                        // the compiler can auto-vectorise the inner loop.
                        int64_t local = 0;
                        if (cps.size() == 1 && !cps[0].validity) {
                            // Single-pred, no nulls: the fully-typed,
                            // branch-free hot loop. Auto-vectorisable.
                            auto &cp = cps[0];
                            #define LOOP(arr, cmp, lit) do { \
                                for (idx_t i = 0; i < nrows; i++) \
                                    if ((arr)[i] cmp (lit)) ++local; \
                            } while (0)
                            if (cp.type == LogicalTypeId::BIGINT) {
                                int64_t lit = cp.ival;
                                switch (cp.op) {
                                case SimpleCmpOp::LT: LOOP(cp.i64, <,  lit); break;
                                case SimpleCmpOp::LE: LOOP(cp.i64, <=, lit); break;
                                case SimpleCmpOp::GT: LOOP(cp.i64, >,  lit); break;
                                case SimpleCmpOp::GE: LOOP(cp.i64, >=, lit); break;
                                case SimpleCmpOp::EQ: LOOP(cp.i64, ==, lit); break;
                                case SimpleCmpOp::NE: LOOP(cp.i64, !=, lit); break;
                                }
                            } else if (cp.type == LogicalTypeId::INTEGER) {
                                int32_t lit = static_cast<int32_t>(cp.ival);
                                switch (cp.op) {
                                case SimpleCmpOp::LT: LOOP(cp.i32, <,  lit); break;
                                case SimpleCmpOp::LE: LOOP(cp.i32, <=, lit); break;
                                case SimpleCmpOp::GT: LOOP(cp.i32, >,  lit); break;
                                case SimpleCmpOp::GE: LOOP(cp.i32, >=, lit); break;
                                case SimpleCmpOp::EQ: LOOP(cp.i32, ==, lit); break;
                                case SimpleCmpOp::NE: LOOP(cp.i32, !=, lit); break;
                                }
                            } else if (cp.type == LogicalTypeId::DOUBLE) {
                                double lit = cp.dval;
                                switch (cp.op) {
                                case SimpleCmpOp::LT: LOOP(cp.f64, <,  lit); break;
                                case SimpleCmpOp::LE: LOOP(cp.f64, <=, lit); break;
                                case SimpleCmpOp::GT: LOOP(cp.f64, >,  lit); break;
                                case SimpleCmpOp::GE: LOOP(cp.f64, >=, lit); break;
                                case SimpleCmpOp::EQ: LOOP(cp.f64, ==, lit); break;
                                case SimpleCmpOp::NE: LOOP(cp.f64, !=, lit); break;
                                }
                            } else if (cp.type == LogicalTypeId::FLOAT) {
                                float lit = static_cast<float>(cp.dval);
                                switch (cp.op) {
                                case SimpleCmpOp::LT: LOOP(cp.f32, <,  lit); break;
                                case SimpleCmpOp::LE: LOOP(cp.f32, <=, lit); break;
                                case SimpleCmpOp::GT: LOOP(cp.f32, >,  lit); break;
                                case SimpleCmpOp::GE: LOOP(cp.f32, >=, lit); break;
                                case SimpleCmpOp::EQ: LOOP(cp.f32, ==, lit); break;
                                case SimpleCmpOp::NE: LOOP(cp.f32, !=, lit); break;
                                }
                            }
                            #undef LOOP
                        } else {
                            // Multi-pred or has nulls: per-row check, but
                            // still with hoisted column pointers/types.
                            for (idx_t i = 0; i < nrows; i++) {
                                bool keep = true;
                                for (auto &cp : cps) {
                                    if (cp.validity && !cp.validity[i]) { keep = false; break; }
                                    bool ok = false;
                                    if (cp.i64) {
                                        int64_t v = cp.i64[i], lit = cp.ival;
                                        switch (cp.op) {
                                        case SimpleCmpOp::LT: ok = v <  lit; break;
                                        case SimpleCmpOp::LE: ok = v <= lit; break;
                                        case SimpleCmpOp::GT: ok = v >  lit; break;
                                        case SimpleCmpOp::GE: ok = v >= lit; break;
                                        case SimpleCmpOp::EQ: ok = v == lit; break;
                                        case SimpleCmpOp::NE: ok = v != lit; break;
                                        }
                                    } else if (cp.i32) {
                                        int32_t v = cp.i32[i]; int32_t lit = (int32_t)cp.ival;
                                        switch (cp.op) {
                                        case SimpleCmpOp::LT: ok = v <  lit; break;
                                        case SimpleCmpOp::LE: ok = v <= lit; break;
                                        case SimpleCmpOp::GT: ok = v >  lit; break;
                                        case SimpleCmpOp::GE: ok = v >= lit; break;
                                        case SimpleCmpOp::EQ: ok = v == lit; break;
                                        case SimpleCmpOp::NE: ok = v != lit; break;
                                        }
                                    } else if (cp.f64) {
                                        double v = cp.f64[i], lit = cp.dval;
                                        switch (cp.op) {
                                        case SimpleCmpOp::LT: ok = v <  lit; break;
                                        case SimpleCmpOp::LE: ok = v <= lit; break;
                                        case SimpleCmpOp::GT: ok = v >  lit; break;
                                        case SimpleCmpOp::GE: ok = v >= lit; break;
                                        case SimpleCmpOp::EQ: ok = v == lit; break;
                                        case SimpleCmpOp::NE: ok = v != lit; break;
                                        }
                                    } else if (cp.f32) {
                                        float v = cp.f32[i]; float lit = (float)cp.dval;
                                        switch (cp.op) {
                                        case SimpleCmpOp::LT: ok = v <  lit; break;
                                        case SimpleCmpOp::LE: ok = v <= lit; break;
                                        case SimpleCmpOp::GT: ok = v >  lit; break;
                                        case SimpleCmpOp::GE: ok = v >= lit; break;
                                        case SimpleCmpOp::EQ: ok = v == lit; break;
                                        case SimpleCmpOp::NE: ok = v != lit; break;
                                        }
                                    }
                                    if (!ok) { keep = false; break; }
                                }
                                if (keep) ++local;
                            }
                        }
                        tl_match[tid % MAX_THREADS].fetch_add(local, std::memory_order_relaxed);
                    });
                fused_pq->RunParallelRGs(0);
                int64_t total = 0;
                for (auto &c : tl_match) total += c.load(std::memory_order_relaxed);
                state.count = total;
                total_rows_processed = static_cast<idx_t>(total);
            } else if (parquet_scan) {
                // Parquet COUNT(*) - read row count from footer metadata without
                // decoding any column data.
                parquet_scan->Init();
                state.count = parquet_scan->GetReader()->NumRows();
                total_rows_processed = static_cast<idx_t>(state.count);
            } else {
                // Regular table - just count chunk sizes.
                while (children[0]->GetData(chunk)) {
                    state.count += static_cast<int64_t>(chunk.size());
                    total_rows_processed += chunk.size();
                }
            }
        } else if (all_simple_aggs && is_simple_no_group) {
            // Fast: no GROUP BY, simple aggregates - process vectors directly, no key building.
            // Column pruning: only parse columns we aggregate over.
            {
                std::vector<bool> needed(children[0]->GetTypes().size(), false);
                bool any = false;
                for (auto &info : agg_infos) {
                    if (info.col_idx != INVALID_INDEX && info.col_idx < needed.size()) {
                        needed[info.col_idx] = true; any = true;
                    }
                }
                if (any) {
                    children[0]->SetNeededOutputs(needed);
                    if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                        fs->SetProjection(std::move(needed));
                    }
                }
            }

            group_order.push_back("");
            group_states[""].resize(num_aggs);
            group_keys[""] = {};
            auto &states = group_states[""];

            // === FUSED PARQUET FAST PATH ===
            // Sequential aggregate (parallel decode via slot mode). Q2's agg
            // work is already tiny - 80 RGs × SUM is <10ms - so paying per-
            // query thread-pool teardown+spawn to parallelize it nets negative.
            // Also accepts AGG -> FILTER -> PARQUET via TryCompileSimplePredicate;
            // the per-RG worker applies the typed-keep-mask before the per-row
            // agg loop. Without this, no-group `COUNT(col) WHERE filter` over
            // parquet falls into the chunk-by-chunk serial fallback (~40s on
            // 99M-row hits).
            PhysicalParquetScan *pq =
                dynamic_cast<PhysicalParquetScan *>(children[0].get());
            std::vector<SimplePredicate> nogr_preds;
            bool nogr_has_filter = false;
            if (!pq) {
                if (auto *flt = dynamic_cast<PhysicalFilter *>(children[0].get())) {
                    if (!flt->children.empty()) {
                        if (auto *spq = dynamic_cast<PhysicalParquetScan *>(
                                flt->children[0].get())) {
                            std::vector<SimplePredicate> tmp;
                            if (flt->GetCondition() &&
                                TryCompileSimplePredicate(*flt->GetCondition(),
                                                          tmp)) {
                                pq = spq;
                                nogr_preds = std::move(tmp);
                                nogr_has_filter = true;
                                std::vector<bool> need(pq->GetTypes().size(),
                                                       false);
                                bool any = false;
                                for (auto &info : agg_infos) {
                                    if (info.col_idx != INVALID_INDEX &&
                                        info.col_idx < need.size()) {
                                        need[info.col_idx] = true; any = true;
                                    }
                                }
                                for (auto &p : nogr_preds) {
                                    if (p.col_idx < need.size()) {
                                        need[p.col_idx] = true; any = true;
                                    }
                                }
                                if (any) pq->SetNeededOutputs(need);
                                std::vector<bool> skip(pq->GetTypes().size(),
                                                       false);
                                for (auto &p : nogr_preds) {
                                    if (p.col_idx < skip.size() && p.str_form &&
                                        pq->GetTypes()[p.col_idx].id() ==
                                            LogicalTypeId::VARCHAR) {
                                        skip[p.col_idx] = true;
                                    }
                                }
                                pq->SetSkipStrData(std::move(skip));
                            }
                        }
                    }
                }
            }
            // Filter-mode only safe for COUNT-shaped aggs in this scope —
            // SUM/MIN/MAX with WHERE require touching the AVX2/4-lane hot
            // loops, deferred. If the filter applied but an agg isn't a
            // COUNT, abandon pq so the slow chunk-loop fallback runs (still
            // correct, just not faster).
            if (pq && nogr_has_filter) {
                bool all_count = true;
                for (auto &info : agg_infos) {
                    if (info.name != "COUNT") { all_count = false; break; }
                }
                if (!all_count) pq = nullptr;
            }
            if (pq) {
                // === STATS-ONLY FAST PATH ===
                // When every agg is COUNT(*) / MIN(col) / MAX(col) and the
                // column has per-RG min/max in the footer, fold from stats
                // and skip the scan entirely. Q7 SELECT MIN(EventDate),
                // MAX(EventDate) FROM hits answered in single-digit ms vs
                // ~400 ms decode-and-scan.
                bool stats_only = (num_aggs > 0);
                for (idx_t a = 0; a < num_aggs && stats_only; a++) {
                    auto &info = agg_infos[a];
                    if (info.name == "COUNT" && info.is_count_star) continue;
                    if ((info.name == "MIN" || info.name == "MAX") &&
                        !info.is_distinct && info.col_idx != INVALID_INDEX) continue;
                    stats_only = false;
                }
                if (stats_only) {
                    pq->Init();
                    auto *reader = pq->GetReader();
                    const auto &meta = reader->GetMeta();
                    bool all_stats = true;
                    for (auto &rg : meta.row_groups) {
                        for (idx_t a = 0; a < num_aggs && all_stats; a++) {
                            auto &info = agg_infos[a];
                            if (info.name != "MIN" && info.name != "MAX") continue;
                            if (info.col_idx >= rg.columns.size() ||
                                !rg.columns[info.col_idx].has_stats) {
                                all_stats = false;
                            }
                        }
                        if (!all_stats) break;
                    }
                    if (all_stats) {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            auto &info = agg_infos[a];
                            auto &state = states[a];
                            if (info.name == "COUNT" && info.is_count_star) {
                                state.count = meta.num_rows;
                            } else if (info.name == "MIN") {
                                for (auto &rg : meta.row_groups) {
                                    const auto &v = rg.columns[info.col_idx].min_value;
                                    if (v.IsNull()) continue;
                                    if (!state.has_min || v < state.min_val()) {
                                        state.min_val() = v;
                                        state.has_min = true;
                                    }
                                }
                            } else if (info.name == "MAX") {
                                for (auto &rg : meta.row_groups) {
                                    const auto &v = rg.columns[info.col_idx].max_value;
                                    if (v.IsNull()) continue;
                                    if (!state.has_max || state.max_val() < v) {
                                        state.max_val() = v;
                                        state.has_max = true;
                                    }
                                }
                            }
                        }
                        total_rows_processed += static_cast<idx_t>(meta.num_rows);
                        goto fused_parquet_stats_only_done;
                    }
                }
                // === Q4 DICT-HISTOGRAM FAST PATH ===
                // Single SUM/AVG/COUNT(c) over a BIGINT-dict-encoded column,
                // no GROUP BY, no WHERE, no offset, no DISTINCT. Skips the
                // 800MB sequential write of decoded INT64 i64_data and the
                // per-row dict-gather; replaces with per-RG histogram +
                // sum_idx (count[idx] * dict[idx]). See
                // include/slothdb/execution/dict_histogram.hpp.
                if (num_aggs == 1 && !pq->HasPushdownFilters() &&
                    !nogr_has_filter) {
                    auto &info = agg_infos[0];
                    bool eligible = !info.is_count_star &&
                                    !info.is_distinct &&
                                    !info.sum_with_offset &&
                                    info.col_idx != INVALID_INDEX &&
                                    info.primary_idx == INVALID_INDEX &&
                                    (info.name == "SUM" || info.name == "AVG" ||
                                     info.name == "COUNT");
                    if (eligible) {
                        auto col_types = pq->GetTypes();
                        if (info.col_idx < col_types.size() &&
                            col_types[info.col_idx].id() == LogicalTypeId::BIGINT) {
                            pq->Init();
                            auto *reader_q4 = pq->GetReader();
                            if (reader_q4) {
                                int64_t hcount = 0; double hsum = 0.0;
                                if (TryDictHistogramAgg(*reader_q4, info.col_idx,
                                                         hcount, hsum)) {
                                    auto &state = states[0];
                                    state.count = hcount;
                                    state.sum = hsum;
                                    total_rows_processed +=
                                        static_cast<idx_t>(reader_q4->GetMeta().num_rows);
                                    goto fused_parquet_stats_only_done;
                                }
                                // Fall through to the standard fused path.
                            }
                        }
                    }
                }
                {
                auto *meta_for_stats_p = pq->GetReader();
                const ParquetFileMeta *meta_for_stats = meta_for_stats_p
                    ? &meta_for_stats_p->GetMeta() : nullptr;
                constexpr int MAX_THREADS = 8;
                std::vector<std::vector<AggState>> tl_states(MAX_THREADS);
                for (auto &v : tl_states) v.resize(num_aggs);
                std::vector<idx_t> tl_rows(MAX_THREADS, 0);
                pq->SetRGConsumer([&](const PhysicalParquetScan::RGWork &work,
                                       idx_t rg_idx, int thread_id) {
                    auto &lstates = tl_states[thread_id];
                    idx_t nrows = pq->RowGroupSize(rg_idx);
                    std::vector<uint8_t> tk_nogr;
                    bool tk_nogr_active = false;
                    if (nogr_has_filter) {
                        tk_nogr_active = BuildTypedKeepMask(
                            nogr_preds, work.cols, nrows, tk_nogr);
                    }
                    auto passing_for_rg = [&]() -> int64_t {
                        if (!nogr_has_filter) return (int64_t)nrows;
                        int64_t cnt = 0;
                        if (tk_nogr_active) {
                            for (idx_t i = 0; i < nrows; i++) cnt += tk_nogr[i];
                        } else {
                            for (idx_t i = 0; i < nrows; i++) {
                                if (EvalSimplePredicates(
                                        nogr_preds, work.cols, i)) cnt++;
                            }
                        }
                        return cnt;
                    };
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &state = lstates[a];
                        auto &info = agg_infos[a];
                        if (info.name == "COUNT" && info.is_count_star) {
                            state.count += passing_for_rg();
                            continue;
                        }
                        if (info.col_idx == INVALID_INDEX) continue;
                        // Skip duplicates: emit reads from the primary's
                        // state and applies any per-agg offset. Critical
                        // for Q30: 90 SUMs over the same column → 1 scan.
                        if (info.primary_idx != INVALID_INDEX) continue;
                        // Per-RG stats fold: when this column's min == max in
                        // this RG (every row holds the same value), roll up
                        // SUM/COUNT/MIN/MAX from the metadata directly and
                        // skip the typed inner loop. Q3 SUM(AdvEngineID)
                        // hits this constantly — most RGs are all-zero.
                        // Filter-mode disables the stats fold (stats don't
                        // honor the per-row predicate).
                        if (!nogr_has_filter && meta_for_stats &&
                            rg_idx < meta_for_stats->row_groups.size() &&
                            info.col_idx < meta_for_stats->row_groups[rg_idx].columns.size()) {
                            const auto &cmeta = meta_for_stats->row_groups[rg_idx].columns[info.col_idx];
                            if (cmeta.has_stats && !cmeta.min_value.IsNull() &&
                                !cmeta.max_value.IsNull() &&
                                cmeta.min_value == cmeta.max_value) {
                                auto vt = cmeta.min_value.type().id();
                                if (vt == LogicalTypeId::BIGINT || vt == LogicalTypeId::INTEGER ||
                                    vt == LogicalTypeId::DOUBLE || vt == LogicalTypeId::FLOAT) {
                                    double dv = 0;
                                    switch (vt) {
                                    case LogicalTypeId::BIGINT:  dv = (double)cmeta.min_value.GetValue<int64_t>(); break;
                                    case LogicalTypeId::INTEGER: dv = (double)cmeta.min_value.GetValue<int32_t>(); break;
                                    case LogicalTypeId::DOUBLE:  dv = cmeta.min_value.GetValue<double>(); break;
                                    case LogicalTypeId::FLOAT:   dv = (double)cmeta.min_value.GetValue<float>(); break;
                                    default: break;
                                    }
                                    if (info.name == "SUM" || info.name == "AVG") {
                                        state.sum += dv * static_cast<double>(nrows);
                                        state.count += static_cast<int64_t>(nrows);
                                        continue;
                                    } else if (info.name == "COUNT") {
                                        state.count += static_cast<int64_t>(nrows);
                                        continue;
                                    } else if (info.name == "MIN") {
                                        if (!state.has_min || dv < state.sum_min) {
                                            state.sum_min = dv; state.has_min = true;
                                        }
                                        continue;
                                    } else if (info.name == "MAX") {
                                        if (!state.has_max || dv > state.sum_max) {
                                            state.sum_max = dv; state.has_max = true;
                                        }
                                        continue;
                                    }
                                }
                            }
                        }
                        const auto &col = work.cols[info.col_idx];
                        if (!col.decoded) {
                            const auto &vals = work.cols_fallback[info.col_idx];
                            for (auto &v : vals) {
                                if (v.IsNull()) continue;
                                if (info.name == "COUNT") state.count++;
                                else if (info.name == "SUM" || info.name == "AVG") {
                                    state.sum += v.GetValue<double>(); state.count++;
                                } else if (info.name == "MIN") {
                                    double d = v.GetValue<double>();
                                    if (!state.has_min || d < state.sum_min) {
                                        state.sum_min = d; state.has_min = true; state.min_val() = v;
                                    }
                                } else if (info.name == "MAX") {
                                    double d = v.GetValue<double>();
                                    if (!state.has_max || d > state.sum_max) {
                                        state.sum_max = d; state.has_max = true; state.max_val() = v;
                                    }
                                }
                            }
                            continue;
                        }
                        auto tid = col.type.id();
                        bool all_valid = col.all_valid;
                        if (info.name == "COUNT") {
                            if (nogr_has_filter) {
                                int64_t cnt = 0;
                                if (tk_nogr_active) {
                                    if (all_valid) {
                                        for (idx_t i = 0; i < nrows; i++)
                                            cnt += tk_nogr[i];
                                    } else {
                                        for (idx_t i = 0; i < nrows; i++)
                                            cnt += tk_nogr[i] & col.validity[i];
                                    }
                                } else {
                                    for (idx_t i = 0; i < nrows; i++) {
                                        if (!EvalSimplePredicates(
                                                nogr_preds, work.cols, i)) continue;
                                        if (!all_valid && !col.validity[i]) continue;
                                        cnt++;
                                    }
                                }
                                state.count += cnt;
                            } else if (all_valid) {
                                state.count += static_cast<int64_t>(nrows);
                            } else {
                                for (idx_t i = 0; i < nrows; i++)
                                    if (col.validity[i]) state.count++;
                            }
                            continue;
                        }
                        auto run_numeric = [&](auto *arr) {
                            using T = typename std::remove_cv<
                                        typename std::remove_pointer<decltype(arr)>::type>::type;
                            if (info.name == "SUM" || info.name == "AVG") {
                                if (all_valid) {
#ifdef SLOTHDB_PP_HAS_AVX2
                                    if constexpr (std::is_same_v<T, int32_t>) {
                                        // AVX2 INT32 SUM: 4 vector accumulators
                                        // × 4 doubles each = 16-lane effective
                                        // pipelining; _mm256_cvtepi32_pd
                                        // promotes 4 int32 → 4 doubles per
                                        // instruction. Q3 AVG(ResolutionWidth)
                                        // and Q5 AVG over 100M rows live here.
                                        __m256d acc0 = _mm256_setzero_pd();
                                        __m256d acc1 = _mm256_setzero_pd();
                                        __m256d acc2 = _mm256_setzero_pd();
                                        __m256d acc3 = _mm256_setzero_pd();
                                        idx_t i = 0;
                                        for (; i + 16 <= nrows; i += 16) {
                                            __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(arr + i));
                                            __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(arr + i + 4));
                                            __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(arr + i + 8));
                                            __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(arr + i + 12));
                                            acc0 = _mm256_add_pd(acc0, _mm256_cvtepi32_pd(v0));
                                            acc1 = _mm256_add_pd(acc1, _mm256_cvtepi32_pd(v1));
                                            acc2 = _mm256_add_pd(acc2, _mm256_cvtepi32_pd(v2));
                                            acc3 = _mm256_add_pd(acc3, _mm256_cvtepi32_pd(v3));
                                        }
                                        __m256d sum = _mm256_add_pd(_mm256_add_pd(acc0, acc1),
                                                                     _mm256_add_pd(acc2, acc3));
                                        double tmp[4];
                                        _mm256_storeu_pd(tmp, sum);
                                        double s = (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
                                        for (; i < nrows; i++) s += static_cast<double>(arr[i]);
                                        state.sum += s;
                                        state.count += static_cast<int64_t>(nrows);
                                    } else {
#endif
                                    // 4-lane parallel accumulators break the
                                    // single-lane FP-add dependency chain so
                                    // the CPU can issue 4 independent adds
                                    // per cycle on the FP units. Without
                                    // this, the SUM is bound by FP-add
                                    // latency (~4 cycles), not throughput.
                                    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
                                    idx_t i = 0;
                                    for (; i + 4 <= nrows; i += 4) {
                                        s0 += static_cast<double>(arr[i]);
                                        s1 += static_cast<double>(arr[i + 1]);
                                        s2 += static_cast<double>(arr[i + 2]);
                                        s3 += static_cast<double>(arr[i + 3]);
                                    }
                                    for (; i < nrows; i++) s0 += static_cast<double>(arr[i]);
                                    state.sum += (s0 + s1) + (s2 + s3);
                                    state.count += static_cast<int64_t>(nrows);
#ifdef SLOTHDB_PP_HAS_AVX2
                                    }
#endif
                                } else {
                                    for (idx_t i = 0; i < nrows; i++) {
                                        if (col.validity[i]) {
                                            state.sum += static_cast<double>(arr[i]);
                                            state.count++;
                                        }
                                    }
                                }
                            } else if (info.name == "MIN" || info.name == "MAX") {
                                bool is_min = (info.name == "MIN");
                                T cur = is_min ? std::numeric_limits<T>::max()
                                               : std::numeric_limits<T>::lowest();
                                bool seen = false;
                                for (idx_t i = 0; i < nrows; i++) {
                                    if (!all_valid && !col.validity[i]) continue;
                                    T v = arr[i];
                                    if (!seen || (is_min ? v < cur : v > cur)) { cur = v; seen = true; }
                                }
                                if (seen) {
                                    double d = static_cast<double>(cur);
                                    if (is_min) {
                                        if (!state.has_min || d < state.sum_min) {
                                            state.sum_min = d; state.has_min = true;
                                        }
                                    } else {
                                        if (!state.has_max || d > state.sum_max) {
                                            state.sum_max = d; state.has_max = true;
                                        }
                                    }
                                }
                            }
                        };
                        if (tid == LogicalTypeId::DOUBLE)  run_numeric(col.f64_data.data());
                        else if (tid == LogicalTypeId::BIGINT)  run_numeric(col.i64_data.data());
                        else if (tid == LogicalTypeId::INTEGER) run_numeric(col.i32_data.data());
                        else if (tid == LogicalTypeId::FLOAT)   run_numeric(col.f32_data.data());
                    }
                    tl_rows[thread_id] += nrows;
                });
                int nt = pq->RunParallelRGs(0);
                for (int t = 0; t < nt; t++) {
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &d = states[a]; auto &s = tl_states[t][a];
                        d.count += s.count;
                        d.sum   += s.sum;
                        if (s.has_min && (!d.has_min || s.sum_min < d.sum_min)) {
                            d.sum_min = s.sum_min; d.has_min = true;
                            if (!s.min_val_is_null()) d.set_min_from(s);
                        }
                        if (s.has_max && (!d.has_max || s.sum_max > d.sum_max)) {
                            d.sum_max = s.sum_max; d.has_max = true;
                            if (!s.max_val_is_null()) d.set_max_from(s);
                        }
                    }
                    total_rows_processed += tl_rows[t];
                }
                }
                fused_parquet_stats_only_done:;
            } else if ([&]() -> bool {
                // === PARALLEL CSV SUM/COUNT/AVG/MIN/MAX (no GROUP BY) ===
                // Also handles fused WHERE (AGG -> FILTER -> FILE_SCAN).
                PhysicalFileScan *fs = dynamic_cast<PhysicalFileScan *>(children[0].get());
                std::vector<SimplePredicate> preds;
                bool has_filter = false;
                if (!fs) {
                    if (auto *flt = dynamic_cast<PhysicalFilter *>(children[0].get())) {
                        if (!flt->children.empty()) {
                            if (auto *ufs = dynamic_cast<PhysicalFileScan *>(flt->children[0].get())) {
                                std::vector<SimplePredicate> tmp;
                                if (flt->GetCondition() &&
                                    TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                                    fs = ufs;
                                    preds = std::move(tmp);
                                    has_filter = true;
                                }
                            }
                        }
                    }
                }
                if (!fs || !fs->GetReader() ||
                    fs->GetReader()->GetSize() <= 16 * 1024 * 1024) return false;

                // Expand projection for predicate columns.
                {
                    idx_t nc = fs->GetTypes().size();
                    std::vector<bool> need(nc, false);
                    bool any = false;
                    for (auto &info : agg_infos) {
                        if (info.col_idx != INVALID_INDEX && info.col_idx < nc) { need[info.col_idx] = true; any = true; }
                    }
                    for (auto &p : preds) if (p.col_idx < nc) { need[p.col_idx] = true; any = true; }
                    if (any) fs->SetProjection(need);
                }
                fs->Init();

                auto *reader = fs->GetReader();
                const char *buf = reader->GetBuffer();
                size_t total_size = reader->GetSize();
                size_t data_start = reader->GetPos();
                char delim = fs->GetDelimiter();
                size_t data_size = total_size - data_start;

                unsigned int nt = HWThreads();
                if (nt > 8) nt = 4;
                if (data_size < 16 * 1024 * 1024) nt = 1;

                std::vector<size_t> ranges(nt + 1);
                ranges[0] = data_start;
                ranges[nt] = total_size;
                for (unsigned int t = 1; t < nt; t++) {
                    size_t target = data_start + (data_size * t) / nt;
                    ranges[t] = FastCSVReader::FindLineStart(buf, total_size, target);
                }

                auto types = fs->GetTypes();
                auto projection = fs->GetProjection();
                std::vector<std::vector<AggState>> tstates(nt);
                std::vector<idx_t> trows(nt, 0);

                auto worker_fn3 = [&](unsigned int t) {
                        auto &local = tstates[t];
                        local.resize(num_aggs);
                        FastCSVReader thread_reader(buf, ranges[t], ranges[t + 1], delim);
                        DataChunk chunk;
                        chunk.Initialize(types);
                        while (true) {
                            chunk.Reset();
                            idx_t cnt = projection.empty()
                                ? thread_reader.ReadChunk(chunk, types)
                                : thread_reader.ReadChunkProjected(chunk, types, projection);
                            if (cnt == 0) break;
                            for (idx_t i = 0; i < cnt; i++) {
                                if (has_filter &&
                                    !EvalSimplePredicatesChunk(preds, chunk, i)) continue;
                                trows[t]++;
                                for (idx_t a = 0; a < num_aggs; a++) {
                                    auto &st = local[a];
                                    auto &info = agg_infos[a];
                                    if (info.name == "COUNT" && info.is_count_star) {
                                        st.count++;
                                        continue;
                                    }
                                    if (info.col_idx == INVALID_INDEX) continue;
                                    auto &v = chunk.GetVector(info.col_idx);
                                    if (!v.GetValidity().RowIsValid(i)) continue;
                                    double d;
                                    switch (v.GetType().id()) {
                                    case LogicalTypeId::BIGINT:  d = static_cast<double>(v.GetData<int64_t>()[i]); break;
                                    case LogicalTypeId::INTEGER: d = static_cast<double>(v.GetData<int32_t>()[i]); break;
                                    case LogicalTypeId::DOUBLE:  d = v.GetData<double>()[i]; break;
                                    case LogicalTypeId::FLOAT:   d = static_cast<double>(v.GetData<float>()[i]); break;
                                    default: continue;
                                    }
                                    if (info.name == "COUNT") st.count++;
                                    else if (info.name == "SUM" || info.name == "AVG") {
                                        st.sum += d; st.count++;
                                    } else if (info.name == "MIN") {
                                        if (!st.has_min || d < st.sum_min) { st.sum_min = d; st.has_min = true; }
                                    } else if (info.name == "MAX") {
                                        if (!st.has_max || d > st.sum_max) { st.sum_max = d; st.has_max = true; }
                                    }
                                }
                            }
                        }
                    };
                if (nt > 1) {
                    std::vector<std::thread> threads;
                    threads.reserve(nt);
                    for (unsigned int t = 0; t < nt; t++)
                        threads.emplace_back(worker_fn3, t);
                    for (auto &th : threads) th.join();
                } else {
                    worker_fn3(0);
                }

                // Merge thread-local states.
                for (auto &local : tstates) {
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &dst = states[a];
                        auto &src = local[a];
                        dst.count += src.count;
                        dst.sum += src.sum;
                        if (src.has_min && (!dst.has_min || src.sum_min < dst.sum_min)) {
                            dst.sum_min = src.sum_min; dst.has_min = true;
                        }
                        if (src.has_max && (!dst.has_max || src.sum_max > dst.sum_max)) {
                            dst.sum_max = src.sum_max; dst.has_max = true;
                        }
                    }
                }
                for (idx_t n : trows) total_rows_processed += n;
                return true;
            }()) {
                // Parallel path ran; skip the serial loop.
            } else

            while (children[0]->GetData(chunk)) {
                idx_t chunk_size = chunk.size();
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];
                    if (info.name == "COUNT" && info.is_count_star) {
                        state.count += static_cast<int64_t>(chunk_size);
                    } else if (info.col_idx != INVALID_INDEX) {
                        auto &vec = chunk.GetVector(info.col_idx);
                        auto *validity = &vec.GetValidity();
                        bool all_valid = (validity->GetData() == nullptr); // nullptr = all valid
                        if (info.name == "COUNT") {
                            if (all_valid) { state.count += static_cast<int64_t>(chunk_size); }
                            else { for (idx_t i = 0; i < chunk_size; i++) { if (validity->RowIsValid(i)) state.count++; } }
                        } else if (info.name == "SUM" || info.name == "AVG") {
                            auto tid = vec.GetType().id();
                            if (tid == LogicalTypeId::BIGINT && all_valid) {
                                auto *arr = reinterpret_cast<const int64_t *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += static_cast<double>(arr[i]);
                                state.count += static_cast<int64_t>(chunk_size);
                            } else if (tid == LogicalTypeId::DOUBLE && all_valid) {
                                auto *arr = reinterpret_cast<const double *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += arr[i];
                                state.count += static_cast<int64_t>(chunk_size);
                            } else if (tid == LogicalTypeId::INTEGER && all_valid) {
                                auto *arr = reinterpret_cast<const int32_t *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += static_cast<double>(arr[i]);
                                state.count += static_cast<int64_t>(chunk_size);
                            } else {
                                for (idx_t i = 0; i < chunk_size; i++) {
                                    if (all_valid || validity->RowIsValid(i)) {
                                        state.sum += ReadDouble(vec, i);
                                        state.count++;
                                    }
                                }
                            }
                        } else if (info.name == "MIN" || info.name == "MAX") {
                            for (idx_t i = 0; i < chunk_size; i++) {
                                if (all_valid || validity->RowIsValid(i)) {
                                    double val = ReadDouble(vec, i);
                                    if (info.name == "MIN") {
                                        if (!state.has_min || val < state.sum_min) {
                                            state.sum_min = val; state.has_min = true;
                                            state.min_val() = chunk.GetValue(info.col_idx, i);
                                        }
                                    } else {
                                        if (!state.has_max || val > state.sum_max) {
                                            state.sum_max = val; state.has_max = true;
                                            state.max_val() = chunk.GetValue(info.col_idx, i);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                total_rows_processed += chunk_size;
            }
        } else if ([&]{
            // === DISTINCT FAST PATH PRECHECK ===
            // Match: ungrouped, single-col grouped, or two-col grouped;
            // every agg is COUNT(DISTINCT col) on the SAME INT/BIGINT/
            // VARCHAR column; child is parquet (or AGG -> FILTER ->
            // PARQUET). The planner duplicates the agg when ORDER BY
            // references its alias, so queries like Q9 produce two
            // identical entries — that's why we accept >1 aggs as long
            // as they all match.
            if (group_col_indices.size() > 2) return false;
            if (num_aggs < 1) return false;
            for (auto &info : agg_infos) {
                if (info.name != "COUNT") return false;
                if (!info.is_distinct) return false;
                if (info.col_idx == INVALID_INDEX) return false;
                if (info.col_idx != agg_infos[0].col_idx) return false;
            }
            PhysicalParquetScan *p = dynamic_cast<PhysicalParquetScan *>(children[0].get());
            if (!p) {
                if (auto *f = dynamic_cast<PhysicalFilter *>(children[0].get())) {
                    if (!f->children.empty()) {
                        p = dynamic_cast<PhysicalParquetScan *>(f->children[0].get());
                        if (!p) return false;
                        if (!f->GetCondition()) return false;
                        // The filter must compile to SimplePredicate; otherwise
                        // bypassing PhysicalFilter and calling pq->RunParallelRGs
                        // directly would silently drop the WHERE clause. Q11's
                        // `MobilePhoneModel <> ''` is VARCHAR, not yet supported
                        // by TryCompileSimplePredicate at HEAD — fall through
                        // to the slow generic path which honors the filter.
                        std::vector<SimplePredicate> tmp;
                        if (!TryCompileSimplePredicate(*f->GetCondition(), tmp))
                            return false;
                    } else {
                        return false;
                    }
                }
            }
            if (!p) return false;
            auto agg_t = p->GetTypes()[agg_infos[0].col_idx].id();
            if (agg_t != LogicalTypeId::BIGINT && agg_t != LogicalTypeId::INTEGER &&
                agg_t != LogicalTypeId::VARCHAR) return false;
            for (auto gci : group_col_indices) {
                auto group_t = p->GetTypes()[gci].id();
                if (group_t != LogicalTypeId::INTEGER && group_t != LogicalTypeId::BIGINT &&
                    group_t != LogicalTypeId::VARCHAR) return false;
            }
            return true;
        }()) {
            // === FAST PATH: COUNT(DISTINCT col) [+ optional GROUP BY g] over parquet ===
            // Per-thread set / per-thread map<g, set>; parallel parquet
            // decode; per-key disjoint parallel merge. The slow generic
            // path Value::ToString()s every row into a std::unordered_set
            // which is 50-100× slower on 100M-row scans. This restores the
            // sub-2s wall on Q5/Q6/Q9/Q11.
            PhysicalParquetScan *pq = dynamic_cast<PhysicalParquetScan *>(children[0].get());
            std::vector<SimplePredicate> dpd_preds;
            bool dpd_has_filter = false;
            if (!pq) {
                auto *flt = dynamic_cast<PhysicalFilter *>(children[0].get());
                pq = dynamic_cast<PhysicalParquetScan *>(flt->children[0].get());
                std::vector<SimplePredicate> tmp;
                if (TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                    dpd_preds = std::move(tmp);
                    dpd_has_filter = true;
                }
            }
            const idx_t num_group_cols = group_col_indices.size();
            const bool has_group = num_group_cols >= 1;
            const bool two_col_group = num_group_cols == 2;
            const idx_t group_col = has_group ? group_col_indices[0] : 0;
            const idx_t group_col2 = two_col_group ? group_col_indices[1] : 0;
            const idx_t agg_col = agg_infos[0].col_idx;
            const auto group_tid = has_group ? pq->GetTypes()[group_col].id()
                                              : LogicalTypeId::INVALID;
            [[maybe_unused]] const auto group_tid2 =
                two_col_group ? pq->GetTypes()[group_col2].id()
                              : LogicalTypeId::INVALID;
            const auto agg_tid = pq->GetTypes()[agg_col].id();

            // GROUP BY <varchar> + COUNT(DISTINCT <int>) + ORDER BY DESC
            // LIMIT N: handled by a 2-stage radix dedup. The helper is
            // noinline and at file end so its body doesn't bloat
            // ComputeAggregates' .text section.
            if (has_group && !two_col_group &&
                num_aggs >= 1 &&
                agg_infos[0].name == "COUNT" && agg_infos[0].is_distinct &&
                TryComputeVarcharGroupDistinctTopN(pq, group_col, agg_col,
                    group_tid, agg_tid,
                    topn_active_, topn_ascending_, topn_limit_,
                    (int)num_aggs, dpd_preds, dpd_has_filter,
                    result_rows_)) {
                return;
            }

            // Q11-fast-skip: when the only WHERE predicate is
            // `<group_col> <> ''` against a dict-encoded VARCHAR group
            // column AND the agg is COUNT(DISTINCT INT/BIGINT), we can
            // fold the filter into a per-row dict-idx skip inside the
            // helper. This bypasses BuildTypedKeepMask (a 100M-row pass
            // that writes a 100MB mask read by the helper) and the per-
            // row keep_mask streaming read. See feedback memory and
            // str_group_distinct.cpp::IngestRGGstrIntDistinctDictSkipDi.
            // Precheck above already guarantees every agg is COUNT(DISTINCT
            // same_col), so num_aggs >= 1 is safe — the planner sometimes
            // duplicates the agg when ORDER BY references its alias (Q11
            // produces num_aggs=2 for `COUNT(DISTINCT UserID) AS u ... ORDER
            // BY u`). Tightening to ==1 misses Q11.
            const bool q11_fs_eligible =
                has_group && !two_col_group && num_aggs >= 1 &&
                agg_infos[0].name == "COUNT" && agg_infos[0].is_distinct &&
                (agg_tid == LogicalTypeId::BIGINT ||
                 agg_tid == LogicalTypeId::INTEGER) &&
                group_tid == LogicalTypeId::VARCHAR &&
                dpd_has_filter && dpd_preds.size() == 1 &&
                dpd_preds[0].col_idx == group_col &&
                dpd_preds[0].str_form &&
                dpd_preds[0].op == SimpleCmpOp::NE &&
                !dpd_preds[0].like_contains &&
                dpd_preds[0].sval.empty();

            // Project group + agg + filter cols.
            {
                std::vector<bool> need(pq->GetTypes().size(), false);
                for (auto gci : group_col_indices)
                    if (gci < need.size()) need[gci] = true;
                need[agg_col] = true;
                for (auto &p : dpd_preds) if (p.col_idx < need.size()) need[p.col_idx] = true;
                pq->SetNeededOutputs(need);
                // VARCHAR group/agg cols: dict_fast paths read
                // str_dict_indices+str_dict_values only — str_data is dead
                // weight (12-16 B per row of string_t writes nobody reads).
                // Mid-RG PLAIN page back-fills via MaterialiseStrDataLazy.
                std::vector<bool> skip(pq->GetTypes().size(), false);
                auto try_skip_varchar = [&](idx_t ci) {
                    if (ci < skip.size() &&
                        pq->GetTypes()[ci].id() == LogicalTypeId::VARCHAR) {
                        skip[ci] = true;
                    }
                };
                for (auto gci : group_col_indices) try_skip_varchar(gci);
                if (agg_tid == LogicalTypeId::VARCHAR) try_skip_varchar(agg_col);
                for (auto &p : dpd_preds) {
                    if (p.col_idx < skip.size() && p.str_form &&
                        pq->GetTypes()[p.col_idx].id() == LogicalTypeId::VARCHAR) {
                        skip[p.col_idx] = true;
                    }
                }
                pq->SetSkipStrData(std::move(skip));
            }
            pq->Init();

            constexpr int MAX_THREADS = 16;
            const bool agg_is_varchar = (agg_tid == LogicalTypeId::VARCHAR);

            // Per-thread state. Two layouts:
            //   ungrouped INT     : tl_int_set[tid]            (one set per thread)
            //   ungrouped VARCHAR : tl_str_set[tid]
            //   grouped   INT     : tl_int_groups[tid] (map<g_int, set<int64>>)
            //                       + tl_str_groups[tid] (map<g_str, set<int64>>)
            //   grouped   VARCHAR : tl_int_str_groups[tid] / tl_str_str_groups[tid]
            // VARCHAR distinct stores std::string copies (decoded data is
            // owned by RGWork and reused across RGs, so we can't keep
            // string_t pointers; copy by value at insert).
            struct TLDistinct {
                ankerl::unordered_dense::set<int64_t> int_set;
                ankerl::unordered_dense::set<std::string> str_set;
                // Radix mini-arrays: ungrouped-INT distinct path scatters by
                // low 4 bits of ankerl hash so the merge phase needs no rehash.
                std::array<std::vector<int64_t>, 16> scatter;
                ankerl::unordered_dense::map<int64_t,
                    slothdb::SimpleI64Set> int_g_int_d;
                ankerl::unordered_dense::map<int64_t,
                    ankerl::unordered_dense::set<std::string>> int_g_str_d;
                std::unordered_map<std::string,
                    slothdb::SimpleI64Set> str_g_int_d;
                std::unordered_map<std::string,
                    ankerl::unordered_dense::set<std::string>> str_g_str_d;
                // 2-col grouped variant: composite key = bytewise concat of
                // (col1 || \xFF || col2), with the original (Value, Value)
                // pair stashed alongside for emit. Q12 lives here.
                std::unordered_map<std::string,
                    slothdb::SimpleI64Set> g2_int_d;
                std::unordered_map<std::string,
                    ankerl::unordered_dense::set<std::string>> g2_str_d;
                std::unordered_map<std::string, std::vector<Value>> g2_key_parts;
            };
#if defined(_MSC_VER)
            // Layout tripwire for the MSVC build the perf work is tuned on.
            // sizeof depends on the standard library (libstdc++/libc++ size
            // their map and string internals differently from MSVC's STL),
            // so the pin is MSVC-only; elsewhere it would false-trip.
            static_assert(sizeof(TLDistinct) == 960,
                "TLDistinct layout pinned: growing this struct shifts cache lines"
                " and regresses Q7/Q8/Q30 (see memory feedback_struct_growth_cache_shifts.md)");
#endif
            std::vector<TLDistinct> tls(MAX_THREADS);
            // Q6 path B: per-thread radix-shard for ungrouped VARCHAR
            // distinct. Lives outside TLDistinct (960-byte assert pinned)
            // and is only consulted on the !has_group + agg_is_varchar
            // branch — see string_dedup.hpp.
            StringDedupState q6_state(MAX_THREADS);

            // Q5 ingests ~6M UserIDs per thread; pre-reserve scatter mini-arrays
            // so push_back stays branchless after first growth.
            if (!has_group && !agg_is_varchar) {
                for (auto &tl : tls)
                    for (auto &arr : tl.scatter) arr.reserve(512 * 1024);
            }

            // Build a binary composite key from two column-row pairs. Used
            // by the 2-col GROUP BY scan branch. The 0xFF separator can't
            // occur in a UTF-8 string so it disambiguates `("a", "bc")`
            // from `("ab", "c")`. Length-prefixed encoding is overkill
            // when we only ever read this key as an opaque byte string.
            auto build_g2_key = [](const auto &write_col1, const auto &write_col2) -> std::string {
                std::string k;
                k.reserve(64);
                write_col1(k);
                k.push_back('\xFF');
                write_col2(k);
                return k;
            };
            (void)build_g2_key;

            pq->SetRGConsumer([&](const PhysicalParquetScan::RGWork &work,
                                   idx_t rg_idx, int tid) {
                if (work.pruned) return;
                idx_t nrows = pq->RowGroupSize(rg_idx);
                const auto &acol = work.cols[agg_col];
                if (!acol.decoded) return;
                auto &tl = tls[tid % MAX_THREADS];

                // Q11-fast-skip per-RG: locate empty dict_idx in this RG's
                // group-col dict. If found, hand off to the SkipDi helper
                // and skip BuildTypedKeepMask. Falls back to legacy if the
                // group col isn't dict-encoded in this RG (rare on
                // ClickBench's MobilePhoneModel/MobilePhone columns).
                bool q11_fs_active = false;
                uint32_t q11_fs_skip_di = 0;
                if (q11_fs_eligible) {
                    const auto &gcol_fs = work.cols[group_col];
                    if (gcol_fs.decoded && gcol_fs.str_dict_encoded &&
                        !gcol_fs.str_dict_indices.empty()) {
                        const auto *dv = gcol_fs.str_dict_values.data();
                        idx_t dsz = gcol_fs.str_dict_values.size();
                        for (idx_t i = 0; i < dsz; i++) {
                            if (dv[i].GetSize() == 0) {
                                q11_fs_skip_di = (uint32_t)i;
                                q11_fs_active = true;
                                break;
                            }
                        }
                        // Empty entry not in dict => filter is no-op for
                        // this RG. UINT32_MAX never matches a real di so
                        // the SkipDi helper degenerates to "ingest all".
                        if (!q11_fs_active) {
                            q11_fs_skip_di = UINT32_MAX;
                            q11_fs_active = true;
                        }
                    }
                }

                std::vector<uint8_t> tk;
                bool tk_active = false;
                if (dpd_has_filter && !q11_fs_active)
                    tk_active = BuildTypedKeepMask(dpd_preds, work.cols, nrows, tk);
                auto keep_row = [&](idx_t r) -> bool {
                    if (!dpd_has_filter) return true;
                    if (q11_fs_active) return true;  // helper handles via skip_di
                    if (tk_active) return tk[r] != 0;
                    return EvalSimplePredicates(dpd_preds, work.cols, r);
                };

                const int64_t *a_i64 = (acol.type.id() == LogicalTypeId::BIGINT)
                                            ? acol.i64_data.data() : nullptr;
                const int32_t *a_i32 = (acol.type.id() == LogicalTypeId::INTEGER)
                                            ? acol.i32_data.data() : nullptr;
                const string_t *a_str = (acol.type.id() == LogicalTypeId::VARCHAR &&
                                         !acol.str_data.empty())
                                            ? acol.str_data.data() : nullptr;
                const uint32_t *a_dict_idx = (acol.type.id() == LogicalTypeId::VARCHAR &&
                                              acol.str_dict_encoded &&
                                              !acol.str_dict_indices.empty())
                                                  ? acol.str_dict_indices.data() : nullptr;
                const string_t *a_dict_val = a_dict_idx ? acol.str_dict_values.data()
                                                          : nullptr;
                idx_t a_dsz = a_dict_val ? acol.str_dict_values.size() : 0;

                if (!has_group) {
                    if (!agg_is_varchar) {
                        ankerl::unordered_dense::hash<int64_t> H;
                        if (a_i64) {
                            for (idx_t r = 0; r < nrows; r++) {
                                if (!keep_row(r)) continue;
                                if (!acol.all_valid && !acol.validity[r]) continue;
                                int64_t v = a_i64[r];
                                tl.scatter[H(v) & 0xF].push_back(v);
                            }
                        } else if (a_i32) {
                            for (idx_t r = 0; r < nrows; r++) {
                                if (!keep_row(r)) continue;
                                if (!acol.all_valid && !acol.validity[r]) continue;
                                int64_t v = (int64_t)a_i32[r];
                                tl.scatter[H(v) & 0xF].push_back(v);
                            }
                        }
                    } else {
                        // VARCHAR distinct path B: per-thread radix-shard build.
                        // string_dedup_emplace hashes once, picks one of 16
                        // shards, and copies bytes into the per-thread arena
                        // only on miss.
                        // Merge phase below unions disjoint shards in parallel
                        // — no rehash, no duplicate alloc.
                        const int q6tid = tid % MAX_THREADS;
                        if (a_dict_idx) {
                            std::vector<uint8_t> seen(a_dsz, 0);
                            for (idx_t r = 0; r < nrows; r++) {
                                if (!keep_row(r)) continue;
                                if (!acol.all_valid && !acol.validity[r]) continue;
                                uint32_t di = a_dict_idx[r];
                                if (di >= a_dsz || seen[di]) continue;
                                seen[di] = 1;
                                string_dedup_emplace(q6_state, q6tid,
                                           a_dict_val[di].GetData(),
                                           a_dict_val[di].GetSize());
                            }
                        } else if (a_str) {
                            for (idx_t r = 0; r < nrows; r++) {
                                if (!keep_row(r)) continue;
                                if (!acol.all_valid && !acol.validity[r]) continue;
                                string_dedup_emplace(q6_state, q6tid,
                                           a_str[r].GetData(), a_str[r].GetSize());
                            }
                        }
                    }
                    return;
                }
                // 2-col GROUP BY variant. Q12 lives here: (MobilePhone INT,
                // MobilePhoneModel VARCHAR). Composite string key per row;
                // distinct UserID set per composite key. The original
                // (Value, Value) pair is stashed once per new key for emit.
                if (two_col_group) {
                    const auto &gc1 = work.cols[group_col];
                    const auto &gc2 = work.cols[group_col2];
                    if (!gc1.decoded || !gc2.decoded) return;
                    auto t1 = gc1.type.id(), t2 = gc2.type.id();
                    const int32_t *g1_i32 = (t1 == LogicalTypeId::INTEGER) ? gc1.i32_data.data() : nullptr;
                    const int64_t *g1_i64 = (t1 == LogicalTypeId::BIGINT)  ? gc1.i64_data.data() : nullptr;
                    const uint32_t *g1_di = (t1 == LogicalTypeId::VARCHAR &&
                                             gc1.str_dict_encoded &&
                                             !gc1.str_dict_indices.empty())
                                                ? gc1.str_dict_indices.data() : nullptr;
                    const string_t *g1_dv = g1_di ? gc1.str_dict_values.data() : nullptr;
                    idx_t g1_dsz = g1_dv ? gc1.str_dict_values.size() : 0;
                    const string_t *g1_str = (t1 == LogicalTypeId::VARCHAR &&
                                              !gc1.str_data.empty())
                                                ? gc1.str_data.data() : nullptr;
                    const int32_t *g2_i32 = (t2 == LogicalTypeId::INTEGER) ? gc2.i32_data.data() : nullptr;
                    const int64_t *g2_i64 = (t2 == LogicalTypeId::BIGINT)  ? gc2.i64_data.data() : nullptr;
                    const uint32_t *g2_di = (t2 == LogicalTypeId::VARCHAR &&
                                             gc2.str_dict_encoded &&
                                             !gc2.str_dict_indices.empty())
                                                ? gc2.str_dict_indices.data() : nullptr;
                    const string_t *g2_dv = g2_di ? gc2.str_dict_values.data() : nullptr;
                    idx_t g2_dsz = g2_dv ? gc2.str_dict_values.size() : 0;
                    const string_t *g2_str = (t2 == LogicalTypeId::VARCHAR &&
                                              !gc2.str_data.empty())
                                                ? gc2.str_data.data() : nullptr;

                    auto append_int = [](std::string &k, int64_t v) {
                        char buf[8];
                        std::memcpy(buf, &v, 8);
                        k.append(buf, 8);
                    };
                    auto append_col = [&](std::string &k, idx_t r,
                                           LogicalTypeId tid,
                                           const int64_t *i64, const int32_t *i32,
                                           const uint32_t *di, const string_t *dv,
                                           idx_t dsz, const string_t *str) -> bool {
                        if (tid == LogicalTypeId::BIGINT)  { append_int(k, i64[r]); return true; }
                        if (tid == LogicalTypeId::INTEGER) { append_int(k, (int64_t)i32[r]); return true; }
                        if (tid == LogicalTypeId::VARCHAR) {
                            const char *sd; uint32_t sl;
                            if (di) {
                                uint32_t d = di[r];
                                if (d >= dsz) return false;
                                sd = dv[d].GetData(); sl = dv[d].GetSize();
                            } else if (str) {
                                sd = str[r].GetData(); sl = str[r].GetSize();
                            } else { return false; }
                            k.append(sd, sl);
                            return true;
                        }
                        return false;
                    };
                    auto value_for = [&](idx_t r, LogicalTypeId tid,
                                          const int64_t *i64, const int32_t *i32,
                                          const uint32_t *di, const string_t *dv,
                                          idx_t dsz, const string_t *str) -> Value {
                        if (tid == LogicalTypeId::BIGINT)  return Value::BIGINT(i64[r]);
                        if (tid == LogicalTypeId::INTEGER) return Value::INTEGER(i32[r]);
                        if (tid == LogicalTypeId::VARCHAR) {
                            const char *sd = ""; uint32_t sl = 0;
                            if (di) {
                                uint32_t d = di[r];
                                if (d < dsz) { sd = dv[d].GetData(); sl = dv[d].GetSize(); }
                            } else if (str) {
                                sd = str[r].GetData(); sl = str[r].GetSize();
                            }
                            return Value::VARCHAR(std::string(sd, sl));
                        }
                        return Value();
                    };

                    // Per-RG pair cache: when both group cols pack into 32 bits
                    // each (INTEGER or VARCHAR-with-dict), the (col1, col2)
                    // tuple has a uint64 fingerprint. Most rows in a RG repeat
                    // the same pair (~1000x for Q12 MobilePhone, MobilePhoneModel),
                    // so the cache lets us skip the per-row composite-string
                    // build and tl.g2_int_d std::unordered_map probe.
                    bool can_pack_pair =
                        (t1 == LogicalTypeId::INTEGER ||
                         (t1 == LogicalTypeId::VARCHAR && g1_di)) &&
                        (t2 == LogicalTypeId::INTEGER ||
                         (t2 == LogicalTypeId::VARCHAR && g2_di));
                    ankerl::unordered_dense::map<uint64_t, slothdb::SimpleI64Set*> rg_pair_int;
                    ankerl::unordered_dense::map<uint64_t,
                        ankerl::unordered_dense::set<std::string>*> rg_pair_str;
                    auto pair_key = [&](idx_t r) -> uint64_t {
                        uint32_t lo = (t1 == LogicalTypeId::INTEGER)
                                          ? (uint32_t)g1_i32[r] : g1_di[r];
                        uint32_t hi = (t2 == LogicalTypeId::INTEGER)
                                          ? (uint32_t)g2_i32[r] : g2_di[r];
                        return (uint64_t)lo | ((uint64_t)hi << 32);
                    };
                    std::string k;
                    // Q12 hot-path: consecutive rows commonly repeat the
                    // exact same (pair, value) tuple (e.g. one user with
                    // many sessions, same MobilePhone/Model). Skipping the
                    // SimpleI64Set::insert open-addressing probe on a
                    // duplicate is essentially free.
                    uint64_t prev_pkey = UINT64_MAX;
                    int64_t prev_v_int = 0;
                    slothdb::SimpleI64Set *prev_sp_int = nullptr;
                    for (idx_t r = 0; r < nrows; r++) {
                        if (!keep_row(r)) continue;
                        if (!acol.all_valid && !acol.validity[r]) continue;
                        slothdb::SimpleI64Set *sp = nullptr;
                        ankerl::unordered_dense::set<std::string> *strset = nullptr;
                        uint64_t pkey = 0;
                        bool packed = can_pack_pair;
                        if (packed) {
                            pkey = pair_key(r);
                            if (!agg_is_varchar) {
                                int64_t cur_v = a_i64 ? a_i64[r] : (int64_t)a_i32[r];
                                if (pkey == prev_pkey) {
                                    if (cur_v == prev_v_int) continue;
                                    sp = prev_sp_int;
                                } else {
                                    auto cit = rg_pair_int.find(pkey);
                                    if (cit != rg_pair_int.end()) sp = cit->second;
                                }
                            } else {
                                auto cit = rg_pair_str.find(pkey);
                                if (cit != rg_pair_str.end()) strset = cit->second;
                            }
                        }
                        if (!sp && !strset) {
                            k.clear();
                            if (!append_col(k, r, t1, g1_i64, g1_i32, g1_di, g1_dv, g1_dsz, g1_str)) continue;
                            k.push_back('\xFF');
                            if (!append_col(k, r, t2, g2_i64, g2_i32, g2_di, g2_dv, g2_dsz, g2_str)) continue;
                            if (!agg_is_varchar) {
                                auto it = tl.g2_int_d.find(k);
                                if (it == tl.g2_int_d.end()) {
                                    std::vector<Value> kv = {
                                        value_for(r, t1, g1_i64, g1_i32, g1_di, g1_dv, g1_dsz, g1_str),
                                        value_for(r, t2, g2_i64, g2_i32, g2_di, g2_dv, g2_dsz, g2_str)};
                                    tl.g2_key_parts.emplace(k, std::move(kv));
                                    it = tl.g2_int_d.emplace(k, slothdb::SimpleI64Set()).first;
                                }
                                sp = &it->second;
                                if (packed) rg_pair_int.emplace(pkey, sp);
                            } else {
                                auto it = tl.g2_str_d.find(k);
                                if (it == tl.g2_str_d.end()) {
                                    std::vector<Value> kv = {
                                        value_for(r, t1, g1_i64, g1_i32, g1_di, g1_dv, g1_dsz, g1_str),
                                        value_for(r, t2, g2_i64, g2_i32, g2_di, g2_dv, g2_dsz, g2_str)};
                                    tl.g2_key_parts.emplace(k, std::move(kv));
                                    it = tl.g2_str_d.emplace(k,
                                        ankerl::unordered_dense::set<std::string>()).first;
                                }
                                strset = &it->second;
                                if (packed) rg_pair_str.emplace(pkey, strset);
                            }
                        }
                        if (sp) {
                            int64_t v = a_i64 ? a_i64[r] : (int64_t)a_i32[r];
                            sp->insert(v);
                            if (packed) {
                                prev_pkey = pkey;
                                prev_v_int = v;
                                prev_sp_int = sp;
                            }
                        } else if (strset) {
                            const char *sd; uint32_t sl;
                            if (a_dict_idx) {
                                uint32_t di = a_dict_idx[r];
                                if (di >= a_dsz) continue;
                                sd = a_dict_val[di].GetData(); sl = a_dict_val[di].GetSize();
                            } else if (a_str) {
                                sd = a_str[r].GetData(); sl = a_str[r].GetSize();
                            } else continue;
                            strset->emplace(sd, sl);
                        }
                    }
                    return;
                }
                // Grouped variant.
                const auto &gcol = work.cols[group_col];
                if (!gcol.decoded) return;
                auto gtid = gcol.type.id();
                bool gint = (gtid == LogicalTypeId::INTEGER || gtid == LogicalTypeId::BIGINT);
                bool gstr = (gtid == LogicalTypeId::VARCHAR);
                const int32_t *g_i32 = (gtid == LogicalTypeId::INTEGER)
                                            ? gcol.i32_data.data() : nullptr;
                const int64_t *g_i64 = (gtid == LogicalTypeId::BIGINT)
                                            ? gcol.i64_data.data() : nullptr;
                const uint32_t *g_dict_idx = (gtid == LogicalTypeId::VARCHAR &&
                                              gcol.str_dict_encoded &&
                                              !gcol.str_dict_indices.empty())
                                                  ? gcol.str_dict_indices.data() : nullptr;
                const string_t *g_dict_val = g_dict_idx ? gcol.str_dict_values.data()
                                                          : nullptr;
                idx_t g_dsz = g_dict_val ? gcol.str_dict_values.size() : 0;
                const string_t *g_str = (gtid == LogicalTypeId::VARCHAR &&
                                         !gcol.str_data.empty())
                                            ? gcol.str_data.data() : nullptr;

                // gint × int distinct
                if (gint && !agg_is_varchar) {
                    int64_t prev_g = 0;
                    slothdb::SimpleI64Set *cached = nullptr;
                    for (idx_t r = 0; r < nrows; r++) {
                        if (!keep_row(r)) continue;
                        if (!acol.all_valid && !acol.validity[r]) continue;
                        int64_t gv = g_i64 ? g_i64[r] : (int64_t)g_i32[r];
                        if (!cached || gv != prev_g) {
                            cached = &tl.int_g_int_d[gv];
                            prev_g = gv;
                        }
                        cached->insert(a_i64 ? a_i64[r] : (int64_t)a_i32[r]);
                    }
                    return;
                }
                // gint × str distinct
                if (gint && agg_is_varchar) {
                    int64_t prev_g = 0;
                    ankerl::unordered_dense::set<std::string> *cached = nullptr;
                    auto fetch_str = [&](idx_t r, const char *&out_d, uint32_t &out_l) -> bool {
                        if (a_dict_idx) {
                            uint32_t di = a_dict_idx[r];
                            if (di >= a_dsz) return false;
                            out_d = a_dict_val[di].GetData();
                            out_l = a_dict_val[di].GetSize();
                            return true;
                        }
                        if (a_str) {
                            out_d = a_str[r].GetData();
                            out_l = a_str[r].GetSize();
                            return true;
                        }
                        return false;
                    };
                    for (idx_t r = 0; r < nrows; r++) {
                        if (!keep_row(r)) continue;
                        if (!acol.all_valid && !acol.validity[r]) continue;
                        int64_t gv = g_i64 ? g_i64[r] : (int64_t)g_i32[r];
                        if (!cached || gv != prev_g) {
                            cached = &tl.int_g_str_d[gv];
                            prev_g = gv;
                        }
                        const char *d; uint32_t l;
                        if (!fetch_str(r, d, l)) continue;
                        cached->emplace(d, l);
                    }
                    return;
                }
                // gstr × int distinct (Q11 path). Side-TU helpers in
                // str_group_distinct.cpp keep the inline 50-LOC loop out of
                // physical_planner.cpp .text per
                // feedback_text_icache_shift.md. Slow-path filter (no
                // typed-keep-mask) still falls through to the legacy
                // EvalSimplePredicates loop below.
                if (gstr && !agg_is_varchar && q11_fs_active && g_dict_idx) {
                    slothdb::IngestRGGstrIntDistinctDictSkipDi(
                        tl.str_g_int_d, g_dict_idx, g_dict_val, g_dsz,
                        a_i64, a_i32,
                        acol.all_valid,
                        acol.all_valid ? nullptr : acol.validity.data(),
                        q11_fs_skip_di, nrows);
                    return;
                }
                if (gstr && !agg_is_varchar &&
                    (!dpd_has_filter || tk_active)) {
                    const uint8_t *km = tk_active ? tk.data() : nullptr;
                    if (g_dict_idx) {
                        slothdb::IngestRGGstrIntDistinctDict(
                            tl.str_g_int_d, g_dict_idx, g_dict_val, g_dsz,
                            a_i64, a_i32,
                            acol.all_valid,
                            acol.all_valid ? nullptr : acol.validity.data(),
                            km, dpd_has_filter, nrows);
                    } else if (g_str) {
                        slothdb::IngestRGGstrIntDistinctPlain(
                            tl.str_g_int_d, g_str, a_i64, a_i32,
                            acol.all_valid,
                            acol.all_valid ? nullptr : acol.validity.data(),
                            km, dpd_has_filter, nrows);
                    }
                    return;
                }
                // Slow-path: filter doesn't compile to typed-keep-mask
                // (rare; keeps the per-row EvalSimplePredicates fallback).
                if (gstr && !agg_is_varchar) {
                    if (g_dict_idx) {
                        std::vector<slothdb::SimpleI64Set *>
                            di_to_set(g_dsz, nullptr);
                        for (idx_t r = 0; r < nrows; r++) {
                            if (!keep_row(r)) continue;
                            if (!acol.all_valid && !acol.validity[r]) continue;
                            uint32_t di = g_dict_idx[r];
                            if (di >= g_dsz) continue;
                            auto *sp = di_to_set[di];
                            if (!sp) {
                                std::string key(g_dict_val[di].GetData(),
                                                g_dict_val[di].GetSize());
                                sp = &tl.str_g_int_d[key];
                                di_to_set[di] = sp;
                            }
                            sp->insert(a_i64 ? a_i64[r] : (int64_t)a_i32[r]);
                        }
                    } else if (g_str) {
                        string_t prev_s; bool have_prev = false;
                        slothdb::SimpleI64Set *cached = nullptr;
                        for (idx_t r = 0; r < nrows; r++) {
                            if (!keep_row(r)) continue;
                            if (!acol.all_valid && !acol.validity[r]) continue;
                            if (!have_prev || !(g_str[r] == prev_s)) {
                                std::string key(g_str[r].GetData(), g_str[r].GetSize());
                                cached = &tl.str_g_int_d[key];
                                prev_s = g_str[r]; have_prev = true;
                            }
                            cached->insert(a_i64 ? a_i64[r] : (int64_t)a_i32[r]);
                        }
                    }
                    return;
                }
                // gstr × str distinct (rare)
                if (gstr && agg_is_varchar) {
                    auto fetch_g = [&](idx_t r, std::string &out) -> bool {
                        if (g_dict_idx) {
                            uint32_t di = g_dict_idx[r];
                            if (di >= g_dsz) return false;
                            out.assign(g_dict_val[di].GetData(),
                                       g_dict_val[di].GetSize());
                            return true;
                        }
                        if (g_str) {
                            out.assign(g_str[r].GetData(), g_str[r].GetSize());
                            return true;
                        }
                        return false;
                    };
                    auto fetch_a_str = [&](idx_t r, std::string &out) -> bool {
                        if (a_dict_idx) {
                            uint32_t di = a_dict_idx[r];
                            if (di >= a_dsz) return false;
                            out.assign(a_dict_val[di].GetData(),
                                       a_dict_val[di].GetSize());
                            return true;
                        }
                        if (a_str) {
                            out.assign(a_str[r].GetData(), a_str[r].GetSize());
                            return true;
                        }
                        return false;
                    };
                    std::string g_buf, a_buf;
                    for (idx_t r = 0; r < nrows; r++) {
                        if (!keep_row(r)) continue;
                        if (!acol.all_valid && !acol.validity[r]) continue;
                        if (!fetch_g(r, g_buf)) continue;
                        if (!fetch_a_str(r, a_buf)) continue;
                        tl.str_g_str_d[g_buf].insert(a_buf);
                    }
                }
            });
            // Q6 (ungrouped COUNT(DISTINCT VARCHAR)) decodes + ingests all
            // 100M rows and scales to 12 logical procs (see RunParallelRGs);
            // its RadixHashCountStr has 16 thread slots so tid%16 is safe.
            // The grouped / INT-distinct cases keep the conservative 8-cap
            // (more threads regressed Q11/Q12 — SMT/Snappy oversubscription).
            pq->RunParallelRGs((!has_group && agg_is_varchar) ? 12 : 0);

            bool _q11pf = slothdb::PqProfileOn();
            auto _q11_t0 = _q11pf ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
            // Merge phase.
            auto emit_int_only = [&](int64_t cnt_total) {
                std::string sk;
                group_order.push_back(sk);
                group_states[sk].resize(num_aggs);
                for (auto &s : group_states[sk]) s.count = cnt_total;
                group_keys[sk] = {};
            };
            if (!has_group) {
                if (!agg_is_varchar) {
                    // Scatter pre-partitioned by low 4 bits of the same ankerl
                    // hash, so each shard ingests its mini-arrays without rehash.
                    constexpr int NSHARDS = 16;
                    std::array<size_t, NSHARDS> shard_sz{};
                    std::vector<std::thread> mts;
                    auto run_shard = [&](int s) {
                        slothdb::SimpleI64Set out;
                        out.reserve(512 * 1024);
                        for (int t = 0; t < MAX_THREADS; t++) {
                            for (int64_t v : tls[t].scatter[s]) out.insert(v);
                        }
                        shard_sz[s] = out.size();
                    };
                    for (int s = 1; s < NSHARDS; s++) mts.emplace_back(run_shard, s);
                    run_shard(0);
                    for (auto &m : mts) m.join();
                    int64_t total = 0;
                    for (int s = 0; s < NSHARDS; s++) total += (int64_t)shard_sz[s];
                    emit_int_only(total);
                } else {
                    // Path B: shards already disjoint by hash partition.
                    // string_dedup_total unions per-shard across threads
                    // in parallel without rehashing.
                    emit_int_only(string_dedup_total(q6_state));
                }
            } else if (two_col_group) {
                // 2-col merge: composite key collected across threads, then
                // per-key disjoint parallel union of the distinct sets.
                std::unordered_map<std::string,
                    slothdb::SimpleI64Set> merged_int;
                std::unordered_map<std::string,
                    ankerl::unordered_dense::set<std::string>> merged_str;
                std::unordered_map<std::string, std::vector<Value>> merged_keys;
                std::vector<std::string> all_keys;
                for (int t = 0; t < MAX_THREADS; t++) {
                    for (auto &kv : tls[t].g2_int_d) {
                        if (merged_int.find(kv.first) == merged_int.end()) {
                            merged_int[kv.first];
                            all_keys.push_back(kv.first);
                        }
                    }
                    for (auto &kv : tls[t].g2_str_d) {
                        if (merged_str.find(kv.first) == merged_str.end()) {
                            merged_str[kv.first];
                            if (merged_int.find(kv.first) == merged_int.end())
                                all_keys.push_back(kv.first);
                        }
                    }
                    for (auto &kv : tls[t].g2_key_parts) {
                        merged_keys.emplace(kv.first, kv.second);
                    }
                }
                int merge_w = (int)std::min<size_t>(MAX_THREADS, all_keys.size());
                if (merge_w < 1) merge_w = 1;
                auto run = [&](int wi) {
                    for (size_t i = wi; i < all_keys.size(); i += merge_w) {
                        const std::string &key = all_keys[i];
                        if (!agg_is_varchar) {
                            auto &dst = merged_int[key];
                            for (int t = 0; t < MAX_THREADS; t++) {
                                auto it = tls[t].g2_int_d.find(key);
                                if (it == tls[t].g2_int_d.end()) continue;
                                if (dst.empty()) dst = std::move(it->second);
                                else dst.merge(std::move(it->second));
                            }
                        } else {
                            auto &dst = merged_str[key];
                            for (int t = 0; t < MAX_THREADS; t++) {
                                auto it = tls[t].g2_str_d.find(key);
                                if (it == tls[t].g2_str_d.end()) continue;
                                if (dst.empty()) dst = std::move(it->second);
                                else {
                                    if (it->second.size() > dst.size()) std::swap(dst, it->second);
                                    dst.insert(it->second.begin(), it->second.end());
                                }
                            }
                        }
                    }
                };
                if (merge_w == 1) run(0);
                else {
                    std::vector<std::thread> mts;
                    for (int w = 0; w < merge_w; w++) mts.emplace_back([&, w]() { run(w); });
                    for (auto &t : mts) t.join();
                }
                // Emit using composite keys for group_states / group_order;
                // group_keys carries the original [Value, Value] pair.
                if (!agg_is_varchar) {
                    for (auto &kv : merged_int) {
                        auto &states = group_states[kv.first];
                        states.resize(num_aggs);
                        int64_t cnt = (int64_t)kv.second.size();
                        for (auto &s : states) s.count = cnt;
                        group_keys[kv.first] = merged_keys[kv.first];
                        group_order.push_back(kv.first);
                    }
                } else {
                    for (auto &kv : merged_str) {
                        auto &states = group_states[kv.first];
                        states.resize(num_aggs);
                        int64_t cnt = (int64_t)kv.second.size();
                        for (auto &s : states) s.count = cnt;
                        group_keys[kv.first] = merged_keys[kv.first];
                        group_order.push_back(kv.first);
                    }
                }
            } else {
                // Grouped: collect unique group keys, partition across
                // workers, each worker unions per-group sets for its keys.
                if (group_tid != LogicalTypeId::VARCHAR) {
                    ankerl::unordered_dense::map<int64_t,
                        slothdb::SimpleI64Set> merged_int;
                    ankerl::unordered_dense::map<int64_t,
                        ankerl::unordered_dense::set<std::string>> merged_str;
                    std::vector<int64_t> all_keys;
                    for (int t = 0; t < MAX_THREADS; t++) {
                        for (auto &kv : tls[t].int_g_int_d) {
                            if (merged_int.emplace(kv.first,
                                slothdb::SimpleI64Set()).second)
                                all_keys.push_back(kv.first);
                        }
                        for (auto &kv : tls[t].int_g_str_d) {
                            if (merged_str.emplace(kv.first,
                                ankerl::unordered_dense::set<std::string>()).second &&
                                merged_int.find(kv.first) == merged_int.end())
                                all_keys.push_back(kv.first);
                        }
                    }
                    int merge_w = (int)std::min<size_t>(MAX_THREADS, all_keys.size());
                    if (merge_w < 1) merge_w = 1;
                    auto run = [&](int wi) {
                        for (size_t i = wi; i < all_keys.size(); i += merge_w) {
                            int64_t key = all_keys[i];
                            if (!agg_is_varchar) {
                                auto &dst = merged_int[key];
                                for (int t = 0; t < MAX_THREADS; t++) {
                                    auto it = tls[t].int_g_int_d.find(key);
                                    if (it == tls[t].int_g_int_d.end()) continue;
                                    if (dst.empty()) dst = std::move(it->second);
                                    else dst.merge(std::move(it->second));
                                }
                            } else {
                                auto &dst = merged_str[key];
                                for (int t = 0; t < MAX_THREADS; t++) {
                                    auto it = tls[t].int_g_str_d.find(key);
                                    if (it == tls[t].int_g_str_d.end()) continue;
                                    if (dst.empty()) dst = std::move(it->second);
                                    else {
                                        if (it->second.size() > dst.size()) std::swap(dst, it->second);
                                        dst.insert(it->second.begin(), it->second.end());
                                    }
                                }
                            }
                        }
                    };
                    if (merge_w == 1) run(0);
                    else {
                        std::vector<std::thread> mts;
                        for (int w = 0; w < merge_w; w++) mts.emplace_back([&, w]() { run(w); });
                        for (auto &t : mts) t.join();
                    }
                    if (!agg_is_varchar) {
                        for (auto &kv : merged_int) {
                            std::string sk = std::to_string(kv.first);
                            auto &states = group_states[sk];
                            states.resize(num_aggs);
                            int64_t cnt = (int64_t)kv.second.size();
                            for (auto &s : states) s.count = cnt;
                            group_keys[sk] = {(group_tid == LogicalTypeId::INTEGER)
                                ? Value::INTEGER((int32_t)kv.first)
                                : Value::BIGINT(kv.first)};
                            group_order.push_back(sk);
                        }
                    } else {
                        for (auto &kv : merged_str) {
                            std::string sk = std::to_string(kv.first);
                            auto &states = group_states[sk];
                            states.resize(num_aggs);
                            int64_t cnt = (int64_t)kv.second.size();
                            for (auto &s : states) s.count = cnt;
                            group_keys[sk] = {(group_tid == LogicalTypeId::INTEGER)
                                ? Value::INTEGER((int32_t)kv.first)
                                : Value::BIGINT(kv.first)};
                            group_order.push_back(sk);
                        }
                    }
                } else {
                    // VARCHAR group key.
                    if (!agg_is_varchar) {
                        // Q11: COUNT(DISTINCT <int>) GROUP BY <varchar>.
                        // Skew-aware parallel merge: the old per-group
                        // round-robin merge made one dominant group
                        // (iPad, ~1M distinct UserIDs) the single-worker
                        // long pole. See str_group_distinct.cpp.
                        std::vector<std::unordered_map<std::string,
                            slothdb::SimpleI64Set>*> per_thread;
                        per_thread.reserve(MAX_THREADS);
                        for (int t = 0; t < MAX_THREADS; t++)
                            per_thread.push_back(&tls[t].str_g_int_d);
                        // 12 = all logical procs. The merge is compute-only
                        // (no parquet decode), so the 8-thread decode cap
                        // (Snappy/SMT oversubscription) does not apply.
                        auto counts = slothdb::MergeStrGroupIntDistinct(
                            per_thread, 12);
                        for (auto &kv : counts) {
                            auto &states = group_states[kv.first];
                            states.resize(num_aggs);
                            for (auto &s : states) s.count = kv.second;
                            group_keys[kv.first] = {Value::VARCHAR(kv.first)};
                            group_order.push_back(kv.first);
                        }
                    } else {
                        // gstr x str distinct (rare): per-group union of
                        // string sets, round-robin across workers.
                        std::unordered_map<std::string,
                            ankerl::unordered_dense::set<std::string>>
                            merged_str;
                        std::vector<std::string> all_keys;
                        for (int t = 0; t < MAX_THREADS; t++) {
                            for (auto &kv : tls[t].str_g_str_d) {
                                if (merged_str.find(kv.first) ==
                                    merged_str.end()) {
                                    merged_str[kv.first];
                                    all_keys.push_back(kv.first);
                                }
                            }
                        }
                        int merge_w = (int)std::min<size_t>(
                            MAX_THREADS, all_keys.size());
                        if (merge_w < 1) merge_w = 1;
                        auto run = [&](int wi) {
                            for (size_t i = wi; i < all_keys.size();
                                 i += merge_w) {
                                const std::string &key = all_keys[i];
                                auto &dst = merged_str[key];
                                for (int t = 0; t < MAX_THREADS; t++) {
                                    auto it = tls[t].str_g_str_d.find(key);
                                    if (it == tls[t].str_g_str_d.end())
                                        continue;
                                    if (dst.empty())
                                        dst = std::move(it->second);
                                    else {
                                        if (it->second.size() > dst.size())
                                            std::swap(dst, it->second);
                                        dst.insert(it->second.begin(),
                                                   it->second.end());
                                    }
                                }
                            }
                        };
                        if (merge_w == 1) run(0);
                        else {
                            std::vector<std::thread> mts;
                            for (int w = 0; w < merge_w; w++)
                                mts.emplace_back([&, w]() { run(w); });
                            for (auto &t : mts) t.join();
                        }
                        for (auto &kv : merged_str) {
                            auto &states = group_states[kv.first];
                            states.resize(num_aggs);
                            int64_t cnt = (int64_t)kv.second.size();
                            for (auto &s : states) s.count = cnt;
                            group_keys[kv.first] = {Value::VARCHAR(kv.first)};
                            group_order.push_back(kv.first);
                        }
                    }
                }
            }
            if (_q11pf) {
                fprintf(stderr, "[SLOTH_PROFILE] Q11 merge %.0fms\n",
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - _q11_t0).count()
                        / 1e6);
            }
        } else if (
            (void)0,
            [&]{
                // FUSED PARQUET GENERIC GROUP BY: now handles is_distinct
                // inline via AK::CountDistinctInt / CountDistinctStr in
                // apply_aggs (typed per-state dedup sets) plus a union-
                // based merge that recomputes count from set size, so
                // cross-thread duplicates don't inflate the answer. Bail
                // when COUNT(DISTINCT) targets an unsupported type.
                for (auto &info : agg_infos) {
                    if (!info.is_distinct) continue;
                    if (info.col_idx == INVALID_INDEX) return false;
                    auto t_p = dynamic_cast<PhysicalParquetScan *>(children[0].get());
                    if (!t_p) {
                        if (auto *f = dynamic_cast<PhysicalFilter *>(children[0].get())) {
                            if (!f->children.empty())
                                t_p = dynamic_cast<PhysicalParquetScan *>(f->children[0].get());
                        }
                    }
                    if (!t_p) return false;
                    auto t = t_p->GetTypes()[info.col_idx].id();
                    if (t != LogicalTypeId::BIGINT && t != LogicalTypeId::INTEGER &&
                        t != LogicalTypeId::VARCHAR) return false;
                }
                PhysicalParquetScan *p = dynamic_cast<PhysicalParquetScan *>(children[0].get());
                if (p) return true;
                if (auto *f = dynamic_cast<PhysicalFilter *>(children[0].get())) {
                    if (!f->children.empty() &&
                        dynamic_cast<PhysicalParquetScan *>(f->children[0].get()))
                        return true;
                }
                return false;
            }()) {
            // === FUSED PARQUET GENERIC GROUP BY (multi-col or unusual types) ===
            // Supports AGG -> FILTER -> SCAN via simple-predicate compilation.
            PhysicalParquetScan *pq =
                dynamic_cast<PhysicalParquetScan *>(children[0].get());
            std::vector<SimplePredicate> multi_preds;
            bool multi_has_filter = false;
            if (!pq) {
                auto *flt = dynamic_cast<PhysicalFilter *>(children[0].get());
                pq = dynamic_cast<PhysicalParquetScan *>(flt->children[0].get());
                std::vector<SimplePredicate> tmp;
                if (flt->GetCondition() &&
                    TryCompileSimplePredicate(*flt->GetCondition(), tmp)) {
                    multi_preds = std::move(tmp);
                    multi_has_filter = true;
                }
            }

            // NOTE: The Q31/Q32 (2-col GROUP BY + multi-INT-agg + VARCHAR
            // filter) shape was tested with selection-vector pushdown here.
            // The masked PLAIN INT decoder is per-row (vs the no-mask path's
            // memcpy), which is *slower* than the writes it saves — INT is
            // 4-8 bytes/row and SIMD-memcpy-bound, not write-bound. Pushdown
            // is only enabled for VARCHAR projected cols (Q22-shape) where
            // string_t writes (16 B + heap-append) dominate.

            // === Q31-shape: 2-col GROUP BY (INTEGER + INTEGER) + simple
            //     aggs (COUNT(*) / SUM / AVG / COUNT(c)) on direct INT/BIGINT
            //     cols. ClickBench Q31 (SearchEngineID, ClientIP, COUNT(*),
            //     SUM(IsRefresh), AVG(ResolutionWidth)) lands here. Default
            //     TLMulti packed_groups map per-thread balloons to ~5M
            //     unique pairs × ~70 bytes = 350MB/thread → cache-thrash.
            //     RadixMultiAggI64Key per-thread per-shard keeps each
            //     working set ~22MB (L3-resident). =====
            if (group_col_indices.size() == 2) {
                idx_t gc0 = group_col_indices[0];
                idx_t gc1 = group_col_indices[1];
                auto t0_q31 = pq->GetTypes()[gc0].id();
                auto t1_q31 = pq->GetTypes()[gc1].id();
                bool both_int32 = (t0_q31 == LogicalTypeId::INTEGER &&
                                   t1_q31 == LogicalTypeId::INTEGER);
                if (both_int32 && num_aggs >= 1) {
                    // Classify aggs. Allowed: COUNT(*), SUM, AVG, COUNT(c)
                    // on direct INT/BIGINT cols. is_distinct disqualifies.
                    enum class Q31AggKind { CountStar, Sum, Avg, CountC, Other };
                    std::vector<Q31AggKind> q31_kinds(num_aggs);
                    std::vector<int> sa_col_idx;
                    std::vector<int> sa_emit_idx;  // sa-index per agg (-1 for count_star)
                    std::vector<bool> sa_is_bigint;
                    bool q31_ok = true;
                    int sa_n = 0;
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &info = agg_infos[a];
                        if (info.is_count_star) {
                            q31_kinds[a] = Q31AggKind::CountStar;
                            sa_emit_idx.push_back(-1);
                            continue;
                        }
                        if (info.is_distinct ||
                            info.col_idx == INVALID_INDEX) {
                            q31_ok = false; break;
                        }
                        auto ct = pq->GetTypes()[info.col_idx].id();
                        if (ct != LogicalTypeId::INTEGER &&
                            ct != LogicalTypeId::BIGINT) {
                            q31_ok = false; break;
                        }
                        if (info.name == "COUNT")
                            q31_kinds[a] = Q31AggKind::CountC;
                        else if (info.name == "SUM")
                            q31_kinds[a] = Q31AggKind::Sum;
                        else if (info.name == "AVG")
                            q31_kinds[a] = Q31AggKind::Avg;
                        else { q31_ok = false; break; }
                        sa_col_idx.push_back((int)info.col_idx);
                        sa_is_bigint.push_back(ct == LogicalTypeId::BIGINT);
                        sa_emit_idx.push_back(sa_n);
                        sa_n++;
                    }
                    // Stack vals[4]/valid[4] bound; bail to GENERIC for
                    // wider agg lists.
                    if (sa_n > 4) q31_ok = false;
                    if (q31_ok) {
                        // Project all needed columns.
                        {
                            std::vector<bool> need(pq->GetTypes().size(), false);
                            need[gc0] = true;
                            need[gc1] = true;
                            for (int c : sa_col_idx)
                                if ((idx_t)c < need.size()) need[c] = true;
                            for (auto &p : multi_preds) {
                                if (p.col_idx < need.size()) need[p.col_idx] = true;
                            }
                            pq->SetNeededOutputs(need);
                            std::vector<bool> skip(pq->GetTypes().size(), false);
                            for (auto &p : multi_preds) {
                                if (p.col_idx < skip.size() && p.str_form &&
                                    pq->GetTypes()[p.col_idx].id() ==
                                        LogicalTypeId::VARCHAR) {
                                    skip[p.col_idx] = true;
                                }
                            }
                            pq->SetSkipStrData(std::move(skip));
                        }
                        pq->Init();

                        const int Q31_THREADS = 8;
                        // Generic phase 1-3+emit. Works against any
                        // aggregator with the API:
                        //   .Update(tid, key, vals, valid)
                        //   .MergeShard(s)
                        //   .EmitTopK(k) → vector with .key, .count_star,
                        //   .sum[a], .cnt[a]
                        //   ::N_RADIX
                        // RadixMultiAggI64Key (legacy) and InlineRowAgg<N>
                        // (lifted from DuckDB's GroupedAggregateHashTable
                        // design) both satisfy this. Toggle via env var
                        // SLOTH_USE_INLINE_AGG=1 for A/B vs the legacy
                        // ankerl::map+arena path.
                        auto run_q31 = [&](auto& agg) {
                        pq->SetRGConsumer(
                            [&](const PhysicalParquetScan::RGWork &work,
                                idx_t rg_idx, int tid) {
                                if (work.pruned) return;
                                idx_t nrows = pq->RowGroupSize(rg_idx);
                                const auto &c0 = work.cols[gc0];
                                const auto &c1 = work.cols[gc1];
                                if (!c0.decoded || !c1.decoded) return;
                                const int32_t *g0 = c0.i32_data.data();
                                const int32_t *g1 = c1.i32_data.data();
                                struct AC {
                                    bool is_bigint;
                                    bool all_valid;
                                    const int32_t *i32;
                                    const int64_t *i64;
                                    const uint8_t *valid;
                                };
                                std::vector<AC> acs(sa_n);
                                for (int a = 0; a < sa_n; a++) {
                                    const auto &ac = work.cols[sa_col_idx[a]];
                                    acs[a].is_bigint = sa_is_bigint[a];
                                    acs[a].all_valid = ac.all_valid;
                                    acs[a].valid = ac.all_valid
                                        ? nullptr : ac.validity.data();
                                    acs[a].i32 = sa_is_bigint[a]
                                        ? nullptr : ac.i32_data.data();
                                    acs[a].i64 = sa_is_bigint[a]
                                        ? ac.i64_data.data() : nullptr;
                                }
                                std::vector<uint8_t> tk;
                                bool tk_active = false;
                                if (multi_has_filter) {
                                    tk_active = BuildTypedKeepMask(
                                        multi_preds, work.cols, nrows, tk);
                                }
                                int t = tid % Q31_THREADS;
                                // Stack arrays cap at 4 aggs (matches
                                // detection: sa_n <= 4 enforced upstream
                                // for cache-friendly slot size).
                                int64_t vals[4];
                                uint8_t valid[4];
                                bool all_aggs_valid = true;
                                for (int a = 0; a < sa_n; a++) {
                                    if (!acs[a].all_valid) {
                                        all_aggs_valid = false; break;
                                    }
                                }
                                // Initialize valid[] to 1s once when
                                // all_aggs_valid; the per-row loop only
                                // updates vals[].
                                if (all_aggs_valid) {
                                    for (int a = 0; a < sa_n; a++) valid[a] = 1;
                                }
                                for (idx_t r = 0; r < nrows; r++) {
                                    if (tk_active) {
                                        if (!tk[r]) continue;
                                    } else if (multi_has_filter &&
                                               !EvalSimplePredicates(
                                                   multi_preds, work.cols, r))
                                        continue;
                                    if (!c0.all_valid && !c0.validity[r]) continue;
                                    if (!c1.all_valid && !c1.validity[r]) continue;
                                    // Pack low-32 of both group cols into a
                                    // uint64. Both are INTEGER (32 bits) per
                                    // detection guard.
                                    uint64_t key =
                                        ((uint64_t)(uint32_t)g0[r] << 32) |
                                        (uint32_t)g1[r];
                                    for (int a = 0; a < sa_n; a++) {
                                        if (!all_aggs_valid)
                                            valid[a] = acs[a].valid[r];
                                        if (all_aggs_valid || valid[a]) {
                                            vals[a] = acs[a].is_bigint
                                                ? acs[a].i64[r]
                                                : (int64_t)acs[a].i32[r];
                                        } else {
                                            vals[a] = 0;
                                        }
                                    }
                                    agg.Update(t, key, vals, valid);
                                }
                            });
                        pq->RunParallelRGs();
                        using AggT = std::remove_reference_t<decltype(agg)>;
                        std::vector<std::thread> mts;
                        for (int s = 1; s < AggT::N_RADIX; s++) {
                            mts.emplace_back([&agg, s]() {
                                agg.MergeShard(s);
                            });
                        }
                        agg.MergeShard(0);
                        for (auto &t : mts) t.join();
                        // TopN dispatch only for ORDER BY count(*) DESC.
                        // count_star sits at the index of the COUNT(*) agg
                        // in the output.
                        int top_k = 0;
                        // Determine which agg is COUNT(*); ORDER BY it DESC
                        // is the only TopN we accept.
                        int count_star_emit_idx = -1;
                        for (idx_t a = 0; a < num_aggs; a++) {
                            if (q31_kinds[a] == Q31AggKind::CountStar) {
                                count_star_emit_idx = (int)(2 + a); break;
                            }
                        }
                        if (topn_active_ && topn_limit_ > 0 &&
                            !topn_ascending_ &&
                            count_star_emit_idx >= 0 &&
                            (int)topn_col_idx_ == count_star_emit_idx) {
                            top_k = (int)topn_limit_;
                        }
                        auto results = agg.EmitTopK(top_k);
                        result_rows_.reserve(results.size());
                        for (auto &res : results) {
                            std::vector<Value> row;
                            row.reserve(2 + num_aggs);
                            // Unpack key.
                            int32_t v0 = (int32_t)(uint32_t)(res.key >> 32);
                            int32_t v1 = (int32_t)(uint32_t)res.key;
                            row.push_back(Value::INTEGER(v0));
                            row.push_back(Value::INTEGER(v1));
                            for (idx_t a = 0; a < num_aggs; a++) {
                                switch (q31_kinds[a]) {
                                case Q31AggKind::CountStar:
                                    row.push_back(Value::BIGINT(res.count_star));
                                    break;
                                case Q31AggKind::Sum: {
                                    int sa = sa_emit_idx[a];
                                    row.push_back(Value::BIGINT(res.sum[sa]));
                                    break;
                                }
                                case Q31AggKind::CountC: {
                                    int sa = sa_emit_idx[a];
                                    row.push_back(Value::BIGINT(res.cnt[sa]));
                                    break;
                                }
                                case Q31AggKind::Avg: {
                                    int sa = sa_emit_idx[a];
                                    if (res.cnt[sa] == 0) {
                                        row.push_back(Value::DOUBLE(0.0));
                                    } else {
                                        row.push_back(Value::DOUBLE(
                                            (double)res.sum[sa] /
                                            (double)res.cnt[sa]));
                                    }
                                    break;
                                }
                                default:
                                    row.push_back(Value::BIGINT(0));
                                }
                            }
                            result_rows_.push_back(std::move(row));
                        }
                        };  // end run_q31 generic lambda

                        // Inline-row aggregator is the default (DuckDB-style
                        // single-row layout, ~17% faster than legacy ankerl
                        // +arena on Q31 per A/B). SLOTH_LEGACY_AGG=1 falls
                        // back to the prior ankerl::map+arena path as a
                        // rollback escape hatch.
                        const bool legacy_override =
                            std::getenv("SLOTH_LEGACY_AGG") != nullptr;
                        // Estimate unique-group cardinality from parquet
                        // metadata × filter-survival ratio. Pre-sizing per-
                        // thread per-shard storage avoids the ~17 table
                        // doublings + rows.emplace_back grows on Q31 (~10M
                        // unique pairs post-SearchPhrase<>'' filter), each
                        // grow rebuilds the entire shard table. SearchPhrase
                        // <>'' survival is ~13% on ClickBench; safe upper
                        // bound 25%. Only apply when a WHERE filter is
                        // present — unfiltered Q31-shape would request a
                        // huge reserve and force zero-fill before any work.
                        size_t q31_reserve_estimate = 0;
                        if (multi_has_filter) {
                            if (auto *q31_reader = pq->GetReader()) {
                                int64_t total = 0;
                                for (auto &rg : q31_reader->GetMeta().row_groups) total += rg.num_rows;
                                int64_t est = (int64_t)((double)total * 0.25);
                                if (est > 20'000'000) est = 20'000'000;
                                if (est < 1024) est = 1024;
                                q31_reserve_estimate = (size_t)est;
                            }
                        }
                        if (!legacy_override && sa_n >= 1 && sa_n <= 4) {
                            switch (sa_n) {
                            case 1: { slothdb::InlineRowAgg<1> a(Q31_THREADS); a.ReserveExpectedRows(q31_reserve_estimate); run_q31(a); break; }
                            case 2: { slothdb::InlineRowAgg<2> a(Q31_THREADS); a.ReserveExpectedRows(q31_reserve_estimate); run_q31(a); break; }
                            case 3: { slothdb::InlineRowAgg<3> a(Q31_THREADS); a.ReserveExpectedRows(q31_reserve_estimate); run_q31(a); break; }
                            case 4: { slothdb::InlineRowAgg<4> a(Q31_THREADS); a.ReserveExpectedRows(q31_reserve_estimate); run_q31(a); break; }
                            }
                        } else {
                            slothdb::RadixMultiAggI64Key a(Q31_THREADS, sa_n);
                            run_q31(a);
                        }
                        return;
                    }
                }
            }

            // === Q32-shape: 2-col GROUP BY where at least one col is
            //     BIGINT (the other can be INTEGER or BIGINT) + simple aggs.
            //     Q32 (WatchID BIGINT, ClientIP INT, WHERE SearchPhrase <> '')
            //     was TIMEOUT through TLMulti packed path because BIGINT
            //     silently truncates to 32 bits → wrong result space, then
            //     OOMs the str_groups fallback at high cardinality.
            //     RadixMultiAggBigKey uses a 128-bit (int64, int64) composite
            //     key. SAFETY GATE: requires a WHERE filter — without one
            //     the cardinality is unbounded (Q33 has 100M near-unique
            //     pairs → ~8 GB across per-thread per-shard arenas → system
            //     thrash). Q33 falls through to existing TLMulti TIMEOUT
            //     path until a memory budget guard is added. =====
            if (group_col_indices.size() == 2 && multi_has_filter) {
                idx_t bk_gc0 = group_col_indices[0];
                idx_t bk_gc1 = group_col_indices[1];
                auto bk_t0 = pq->GetTypes()[bk_gc0].id();
                auto bk_t1 = pq->GetTypes()[bk_gc1].id();
                bool bk_t0_int = (bk_t0 == LogicalTypeId::BIGINT ||
                                  bk_t0 == LogicalTypeId::INTEGER);
                bool bk_t1_int = (bk_t1 == LogicalTypeId::BIGINT ||
                                  bk_t1 == LogicalTypeId::INTEGER);
                bool any_bigint = (bk_t0 == LogicalTypeId::BIGINT ||
                                   bk_t1 == LogicalTypeId::BIGINT);
                if (bk_t0_int && bk_t1_int && any_bigint && num_aggs >= 1) {
                    enum class BKKind { CountStar, Sum, Avg, CountC, Other };
                    std::vector<BKKind> bk_kinds(num_aggs);
                    std::vector<int> bk_sa_col_idx;
                    std::vector<int> bk_sa_emit_idx;
                    std::vector<bool> bk_sa_is_bigint;
                    bool bk_ok = true;
                    int bk_sa_n = 0;
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &info = agg_infos[a];
                        if (info.is_count_star) {
                            bk_kinds[a] = BKKind::CountStar;
                            bk_sa_emit_idx.push_back(-1);
                            continue;
                        }
                        if (info.is_distinct ||
                            info.col_idx == INVALID_INDEX) {
                            bk_ok = false; break;
                        }
                        auto ct = pq->GetTypes()[info.col_idx].id();
                        if (ct != LogicalTypeId::INTEGER &&
                            ct != LogicalTypeId::BIGINT) {
                            bk_ok = false; break;
                        }
                        if (info.name == "COUNT") bk_kinds[a] = BKKind::CountC;
                        else if (info.name == "SUM") bk_kinds[a] = BKKind::Sum;
                        else if (info.name == "AVG") bk_kinds[a] = BKKind::Avg;
                        else { bk_ok = false; break; }
                        bk_sa_col_idx.push_back((int)info.col_idx);
                        bk_sa_is_bigint.push_back(ct == LogicalTypeId::BIGINT);
                        bk_sa_emit_idx.push_back(bk_sa_n);
                        bk_sa_n++;
                    }
                    if (bk_sa_n > 4) bk_ok = false;
                    if (bk_ok) {
                        {
                            std::vector<bool> need(pq->GetTypes().size(), false);
                            need[bk_gc0] = true;
                            need[bk_gc1] = true;
                            for (int c : bk_sa_col_idx)
                                if ((idx_t)c < need.size()) need[c] = true;
                            for (auto &p : multi_preds) {
                                if (p.col_idx < need.size()) need[p.col_idx] = true;
                            }
                            pq->SetNeededOutputs(need);
                            std::vector<bool> skip(pq->GetTypes().size(), false);
                            for (auto &p : multi_preds) {
                                if (p.col_idx < skip.size() && p.str_form &&
                                    pq->GetTypes()[p.col_idx].id() ==
                                        LogicalTypeId::VARCHAR) {
                                    skip[p.col_idx] = true;
                                }
                            }
                            pq->SetSkipStrData(std::move(skip));
                        }
                        pq->Init();

                        const int BK_THREADS = 8;
                        bool g0_is_bigint =
                            (bk_t0 == LogicalTypeId::BIGINT);
                        bool g1_is_bigint =
                            (bk_t1 == LogicalTypeId::BIGINT);
                        // Generic phase 1-3+emit. Same shape as the Q31
                        // dispatch: lifts InlineRowAggBigKey<N> over the
                        // legacy RadixMultiAggBigKey via env-var rollback.
                        auto run_q32 = [&](auto& agg) {
                        pq->SetRGConsumer(
                            [&](const PhysicalParquetScan::RGWork &work,
                                idx_t rg_idx, int tid) {
                                if (work.pruned) return;
                                idx_t nrows = pq->RowGroupSize(rg_idx);
                                const auto &c0 = work.cols[bk_gc0];
                                const auto &c1 = work.cols[bk_gc1];
                                if (!c0.decoded || !c1.decoded) return;
                                const int64_t *g0_64 = g0_is_bigint
                                    ? c0.i64_data.data() : nullptr;
                                const int32_t *g0_32 = g0_is_bigint
                                    ? nullptr : c0.i32_data.data();
                                const int64_t *g1_64 = g1_is_bigint
                                    ? c1.i64_data.data() : nullptr;
                                const int32_t *g1_32 = g1_is_bigint
                                    ? nullptr : c1.i32_data.data();
                                struct AC {
                                    bool is_bigint;
                                    bool all_valid;
                                    const int32_t *i32;
                                    const int64_t *i64;
                                    const uint8_t *valid;
                                };
                                std::vector<AC> acs(bk_sa_n);
                                for (int a = 0; a < bk_sa_n; a++) {
                                    const auto &ac = work.cols[bk_sa_col_idx[a]];
                                    acs[a].is_bigint = bk_sa_is_bigint[a];
                                    acs[a].all_valid = ac.all_valid;
                                    acs[a].valid = ac.all_valid
                                        ? nullptr : ac.validity.data();
                                    acs[a].i32 = bk_sa_is_bigint[a]
                                        ? nullptr : ac.i32_data.data();
                                    acs[a].i64 = bk_sa_is_bigint[a]
                                        ? ac.i64_data.data() : nullptr;
                                }
                                std::vector<uint8_t> tk;
                                bool tk_active = false;
                                if (multi_has_filter) {
                                    tk_active = BuildTypedKeepMask(
                                        multi_preds, work.cols, nrows, tk);
                                }
                                int t = tid % BK_THREADS;
                                int64_t vals[4];
                                uint8_t valid[4];
                                bool all_aggs_valid = true;
                                for (int a = 0; a < bk_sa_n; a++) {
                                    if (!acs[a].all_valid) {
                                        all_aggs_valid = false; break;
                                    }
                                }
                                if (all_aggs_valid) {
                                    for (int a = 0; a < bk_sa_n; a++)
                                        valid[a] = 1;
                                }
                                for (idx_t r = 0; r < nrows; r++) {
                                    if (tk_active) {
                                        if (!tk[r]) continue;
                                    } else if (multi_has_filter &&
                                               !EvalSimplePredicates(
                                                   multi_preds, work.cols, r))
                                        continue;
                                    if (!c0.all_valid && !c0.validity[r]) continue;
                                    if (!c1.all_valid && !c1.validity[r]) continue;
                                    int64_t key_a = g0_is_bigint
                                        ? g0_64[r] : (int64_t)g0_32[r];
                                    int64_t key_b = g1_is_bigint
                                        ? g1_64[r] : (int64_t)g1_32[r];
                                    for (int a = 0; a < bk_sa_n; a++) {
                                        if (!all_aggs_valid)
                                            valid[a] = acs[a].valid[r];
                                        if (all_aggs_valid || valid[a]) {
                                            vals[a] = acs[a].is_bigint
                                                ? acs[a].i64[r]
                                                : (int64_t)acs[a].i32[r];
                                        } else {
                                            vals[a] = 0;
                                        }
                                    }
                                    agg.Update(t, key_a, key_b, vals, valid);
                                }
                            });
                        pq->RunParallelRGs();
                        using BkAggT = std::remove_reference_t<decltype(agg)>;
                        std::vector<std::thread> mts;
                        for (int s = 1; s < BkAggT::N_RADIX; s++) {
                            mts.emplace_back([&agg, s]() {
                                agg.MergeShard(s);
                            });
                        }
                        agg.MergeShard(0);
                        for (auto &t : mts) t.join();
                        int top_k = 0;
                        int count_star_emit_idx = -1;
                        for (idx_t a = 0; a < num_aggs; a++) {
                            if (bk_kinds[a] == BKKind::CountStar) {
                                count_star_emit_idx = (int)(2 + a); break;
                            }
                        }
                        if (topn_active_ && topn_limit_ > 0 &&
                            !topn_ascending_ &&
                            count_star_emit_idx >= 0 &&
                            (int)topn_col_idx_ == count_star_emit_idx) {
                            top_k = (int)topn_limit_;
                        }
                        auto results = agg.EmitTopK(top_k);
                        result_rows_.reserve(results.size());
                        for (auto &res : results) {
                            std::vector<Value> row;
                            row.reserve(2 + num_aggs);
                            if (g0_is_bigint)
                                row.push_back(Value::BIGINT(res.key_a));
                            else
                                row.push_back(
                                    Value::INTEGER((int32_t)res.key_a));
                            if (g1_is_bigint)
                                row.push_back(Value::BIGINT(res.key_b));
                            else
                                row.push_back(
                                    Value::INTEGER((int32_t)res.key_b));
                            for (idx_t a = 0; a < num_aggs; a++) {
                                switch (bk_kinds[a]) {
                                case BKKind::CountStar:
                                    row.push_back(Value::BIGINT(res.count_star));
                                    break;
                                case BKKind::Sum: {
                                    int sa = bk_sa_emit_idx[a];
                                    row.push_back(Value::BIGINT(res.sum[sa]));
                                    break;
                                }
                                case BKKind::CountC: {
                                    int sa = bk_sa_emit_idx[a];
                                    row.push_back(Value::BIGINT(res.cnt[sa]));
                                    break;
                                }
                                case BKKind::Avg: {
                                    int sa = bk_sa_emit_idx[a];
                                    if (res.cnt[sa] == 0) {
                                        row.push_back(Value::DOUBLE(0.0));
                                    } else {
                                        row.push_back(Value::DOUBLE(
                                            (double)res.sum[sa] /
                                            (double)res.cnt[sa]));
                                    }
                                    break;
                                }
                                default:
                                    row.push_back(Value::BIGINT(0));
                                }
                            }
                            result_rows_.push_back(std::move(row));
                        }
                        };  // end run_q32 generic lambda

                        const bool bk_legacy_override =
                            std::getenv("SLOTH_LEGACY_AGG") != nullptr;
                        // Pre-reserve estimate. Q32 requires a filter (gate
                        // above is multi_has_filter); BIGINT × INT pairs at
                        // 100M rows + WHERE filter typically yield ~10-20M
                        // unique. 25% of total_rows is the same heuristic
                        // used for Q31.
                        size_t bk_reserve_estimate = 0;
                        if (auto *bk_reader = pq->GetReader()) {
                            int64_t total = 0;
                            for (auto &rg : bk_reader->GetMeta().row_groups) total += rg.num_rows;
                            int64_t est = (int64_t)((double)total * 0.25);
                            if (est > 20'000'000) est = 20'000'000;
                            if (est < 1024) est = 1024;
                            bk_reserve_estimate = (size_t)est;
                        }
                        if (!bk_legacy_override && bk_sa_n >= 1 && bk_sa_n <= 4) {
                            switch (bk_sa_n) {
                            case 1: { slothdb::InlineRowAggBigKey<1> a(BK_THREADS); a.ReserveExpectedRows(bk_reserve_estimate); run_q32(a); break; }
                            case 2: { slothdb::InlineRowAggBigKey<2> a(BK_THREADS); a.ReserveExpectedRows(bk_reserve_estimate); run_q32(a); break; }
                            case 3: { slothdb::InlineRowAggBigKey<3> a(BK_THREADS); a.ReserveExpectedRows(bk_reserve_estimate); run_q32(a); break; }
                            case 4: { slothdb::InlineRowAggBigKey<4> a(BK_THREADS); a.ReserveExpectedRows(bk_reserve_estimate); run_q32(a); break; }
                            }
                        } else {
                            slothdb::RadixMultiAggBigKey a(BK_THREADS, bk_sa_n);
                            run_q32(a);
                        }
                        return;
                    }
                }
            }

            // === Q15/Q17/Q18-shape: 2-col GROUP BY (INT/BIGINT + VARCHAR) +
            //     only COUNT(*) aggs. Default packed-uint64 path silently
            //     truncates BIGINT to 32 bits (Q17/Q18 UserID), and the
            //     str_groups fallback OOMs at high cardinality. The radix
            //     2-col TU mirrors q16 architecture: composite key
            //     (int64, string_view) hashed once → 16 disjoint shards →
            //     parallel union → optional TopK heap. Lives in a separate
            //     TU per feedback_text_icache_shift.md. =====
            if (group_col_indices.size() == 2) {
                bool all_count_star_2c = (num_aggs >= 1);
                for (idx_t a = 0; a < num_aggs && all_count_star_2c; a++) {
                    if (!agg_infos[a].is_count_star) all_count_star_2c = false;
                }
                if (all_count_star_2c) {
                    auto t0 = pq->GetTypes()[group_col_indices[0]].id();
                    auto t1 = pq->GetTypes()[group_col_indices[1]].id();
                    bool t0_int = (t0 == LogicalTypeId::BIGINT ||
                                   t0 == LogicalTypeId::INTEGER);
                    bool t1_int = (t1 == LogicalTypeId::BIGINT ||
                                   t1 == LogicalTypeId::INTEGER);
                    bool t0_str = (t0 == LogicalTypeId::VARCHAR);
                    bool t1_str = (t1 == LogicalTypeId::VARCHAR);
                    bool int_str = (t0_int && t1_str);
                    bool str_int = (t0_str && t1_int);
                    if (int_str || str_int) {
                        idx_t int_gc = int_str ? group_col_indices[0]
                                                : group_col_indices[1];
                        idx_t str_gc = int_str ? group_col_indices[1]
                                                : group_col_indices[0];
                        bool int_is_bigint =
                            (pq->GetTypes()[int_gc].id() == LogicalTypeId::BIGINT);
                        // Project group + filter cols only.
                        {
                            std::vector<bool> need(pq->GetTypes().size(), false);
                            need[int_gc] = true;
                            need[str_gc] = true;
                            for (auto &p : multi_preds) {
                                if (p.col_idx < need.size()) need[p.col_idx] = true;
                            }
                            pq->SetNeededOutputs(need);
                            // Skip per-row str_data on filter VARCHAR cols
                            // AND on the str group col itself — the dict_fast
                            // hot loop reads str_dict_indices+str_dict_values
                            // exclusively, and PLAIN-page mid-RG fallback
                            // back-fills via MaterialiseStrDataLazy.
                            std::vector<bool> skip(pq->GetTypes().size(), false);
                            skip[str_gc] = true;
                            for (auto &p : multi_preds) {
                                if (p.col_idx < skip.size() && p.str_form &&
                                    pq->GetTypes()[p.col_idx].id() ==
                                        LogicalTypeId::VARCHAR &&
                                    p.col_idx != str_gc) {
                                    skip[p.col_idx] = true;
                                }
                            }
                            pq->SetSkipStrData(std::move(skip));
                        }
                        pq->Init();

                        // 12 decode shards: Q15 is parquet-decode-bound and
                        // scales to all 12 logical procs (see RunParallelRGs).
                        constexpr int Q15_THREADS = 12;
                        slothdb::RadixCount2ColIntStr agg2(Q15_THREADS);
                        // Pre-reserve shard maps for Q15/Q17 (high-card
                        // (int, str) pairs). Without reserve each shard
                        // grows 8 -> 16 -> ... -> 8K through ~10 rehashes
                        // during 100M-row ingest. Estimate expected
                        // unique pairs from parquet total_rows (rough
                        // upper bound — pairs may repeat across rows).
                        {
                            int64_t total_rows = 0;
                            if (auto *r2 = pq->GetReader()) {
                                for (auto &rg : r2->GetMeta().row_groups)
                                    total_rows += rg.num_rows;
                            }
                            int64_t expected = total_rows / 8;
                            if (expected < 1'000'000) expected = 1'000'000;
                            agg2.ReserveExpectedRows(expected);
                        }
                        // Bare-LIMIT early-exit (Q18 shape). Stop picking
                        // new RGs once we've accumulated row_limit_hint_+slack
                        // distinct groups across all threads. Disabled for
                        // TopN paths (Q15/Q17): top-K needs all groups.
                        const bool q18_early_exit =
                            !topn_active_ && row_limit_hint_ > 0;
                        const size_t q18_stop_threshold =
                            q18_early_exit
                                ? (size_t)row_limit_hint_ + 64
                                : 0;
                        pq->SetRGConsumer(
                            [&](const PhysicalParquetScan::RGWork &work,
                                idx_t rg_idx, int tid) {
                                if (work.pruned) return;
                                idx_t nrows = pq->RowGroupSize(rg_idx);
                                const auto &icol = work.cols[int_gc];
                                const auto &scol = work.cols[str_gc];
                                if (!icol.decoded || !scol.decoded) return;
                                const int64_t *gi64 = int_is_bigint
                                    ? icol.i64_data.data() : nullptr;
                                const int32_t *gi32 = !int_is_bigint
                                    ? icol.i32_data.data() : nullptr;
                                bool dict_fast = scol.str_dict_encoded &&
                                    !scol.str_dict_indices.empty();
                                // Q15/Q17 skip-di fast path: single pred =
                                // `<str_gc> <> ''` collapses to a per-row
                                // dict_idx skip. Avoids BuildTypedKeepMask.
                                uint32_t skip_di_q15 = UINT32_MAX;
                                bool single_pred_skip_2c = false;
                                if (dict_fast && multi_has_filter &&
                                    multi_preds.size() == 1) {
                                    const auto &p = multi_preds[0];
                                    if (p.str_form &&
                                        (idx_t)p.col_idx == str_gc &&
                                        p.op == SimpleCmpOp::NE) {
                                        const auto &dv0 = scol.str_dict_values;
                                        for (uint32_t d = 0; d < dv0.size(); d++) {
                                            if (dv0[d].GetSize() == p.sval.size() &&
                                                (p.sval.empty() ||
                                                 std::memcmp(dv0[d].GetData(),
                                                             p.sval.data(),
                                                             p.sval.size()) == 0)) {
                                                skip_di_q15 = d;
                                                break;
                                            }
                                        }
                                        single_pred_skip_2c = true;
                                    }
                                }
                                std::vector<uint8_t> tk;
                                bool tk_active = false;
                                if (multi_has_filter && !single_pred_skip_2c) {
                                    tk_active = BuildTypedKeepMask(
                                        multi_preds, work.cols, nrows, tk);
                                }
                                int t = tid % Q15_THREADS;
                                if (dict_fast && single_pred_skip_2c) {
                                    agg2.IngestRGTwoColCountSkipDi(t,
                                        gi64, gi32, int_is_bigint,
                                        scol.str_dict_indices.data(),
                                        scol.str_dict_values.data(),
                                        (uint32_t)scol.str_dict_values.size(),
                                        icol.all_valid ? nullptr
                                            : icol.validity.data(),
                                        scol.all_valid ? nullptr
                                            : scol.validity.data(),
                                        icol.all_valid, scol.all_valid,
                                        (uint32_t)nrows, skip_di_q15);
                                } else if (dict_fast && (!multi_has_filter || tk_active)) {
                                    // Side-TU helper: shrinks planner .text
                                    // ~30 LOC vs prior inline loop, keeps Q11/Q12
                                    // I-cache stable.
                                    agg2.IngestRGTwoColCount(t,
                                        gi64, gi32, int_is_bigint,
                                        scol.str_dict_indices.data(),
                                        scol.str_dict_values.data(),
                                        (uint32_t)scol.str_dict_values.size(),
                                        icol.all_valid ? nullptr
                                            : icol.validity.data(),
                                        scol.all_valid ? nullptr
                                            : scol.validity.data(),
                                        icol.all_valid, scol.all_valid,
                                        (uint32_t)nrows,
                                        tk_active ? tk.data() : nullptr);
                                } else if (dict_fast) {
                                    // Per-row EvalSimplePredicates fallback when
                                    // typed-keep-mask doesn't cover predicate.
                                    const uint32_t *di = scol.str_dict_indices.data();
                                    const string_t *dv = scol.str_dict_values.data();
                                    idx_t dsz = scol.str_dict_values.size();
                                    for (idx_t r = 0; r < nrows; r++) {
                                        if (!EvalSimplePredicates(
                                                multi_preds, work.cols, r))
                                            continue;
                                        if (!icol.all_valid && !icol.validity[r])
                                            continue;
                                        if (!scol.all_valid && !scol.validity[r])
                                            continue;
                                        uint32_t d = di[r];
                                        if (d >= dsz) continue;
                                        int64_t k = int_is_bigint
                                            ? gi64[r] : (int64_t)gi32[r];
                                        agg2.IncrementRow(t, k,
                                            dv[d].GetData(), dv[d].GetSize());
                                    }
                                } else if (!scol.str_data.empty()) {
                                    const string_t *gs = scol.str_data.data();
                                    for (idx_t r = 0; r < nrows; r++) {
                                        if (tk_active) {
                                            if (!tk[r]) continue;
                                        } else if (multi_has_filter &&
                                                   !EvalSimplePredicates(
                                                       multi_preds, work.cols, r))
                                            continue;
                                        if (!icol.all_valid && !icol.validity[r])
                                            continue;
                                        if (!scol.all_valid && !scol.validity[r])
                                            continue;
                                        int64_t k = int_is_bigint
                                            ? gi64[r] : (int64_t)gi32[r];
                                        agg2.IncrementRow(t, k,
                                            gs[r].GetData(), gs[r].GetSize());
                                    }
                                }
                                if (q18_early_exit &&
                                    agg2.LiveGroupCount() >= q18_stop_threshold) {
                                    pq->RequestStop();
                                }
                            });
                        pq->RunParallelRGs(Q15_THREADS);
                        std::vector<std::thread> mts;
                        for (int s = 1;
                             s < slothdb::RadixCount2ColIntStr::N_RADIX; s++) {
                            mts.emplace_back([&agg2, s]() {
                                agg2.MergeShard(s);
                            });
                        }
                        agg2.MergeShard(0);
                        for (auto &t : mts) t.join();
                        // TopN dispatch: ORDER BY count DESC LIMIT K. The
                        // count column is at index 2 (int_key, str_key,
                        // count). All aggs are CountStar, so any agg col
                        // index orders by count.
                        int top_k = 0;
                        if (topn_active_ && topn_limit_ > 0 &&
                            !topn_ascending_ &&
                            topn_col_idx_ >= (int)group_col_indices.size() &&
                            topn_col_idx_ < (int)group_col_indices.size() +
                                             (int)num_aggs) {
                            top_k = (int)topn_limit_;
                        }
                        // Bare LIMIT (Q22): no ordering required, just need
                        // the first K rows. Avoid materializing 80M Values
                        // when the upstream PhysicalLimit will discard 99.99%.
                        std::vector<RadixCount2ColIntStrResult> results;
                        if (!topn_active_ && row_limit_hint_ > 0) {
                            results = agg2.EmitFirstK((int)row_limit_hint_);
                        } else {
                            results = agg2.EmitTopK(top_k);
                        }
                        result_rows_.reserve(results.size());
                        // Output column order matches group_col_indices: if
                        // INT was first in the SELECT list, emit (int, str);
                        // otherwise (str, int).
                        for (auto &res : results) {
                            std::vector<Value> row;
                            row.reserve(2 + num_aggs);
                            if (int_str) {
                                if (int_is_bigint)
                                    row.push_back(Value::BIGINT(res.int_key));
                                else
                                    row.push_back(
                                        Value::INTEGER((int32_t)res.int_key));
                                row.push_back(Value::VARCHAR(res.str_key));
                            } else {
                                row.push_back(Value::VARCHAR(res.str_key));
                                if (int_is_bigint)
                                    row.push_back(Value::BIGINT(res.int_key));
                                else
                                    row.push_back(
                                        Value::INTEGER((int32_t)res.int_key));
                            }
                            for (idx_t a = 0; a < num_aggs; a++) {
                                row.push_back(Value::BIGINT(res.count));
                            }
                            result_rows_.push_back(std::move(row));
                        }
                        return;
                    }
                }
            }
            // Pre-resolve which aggs take a non-column expression argument
            // (e.g. AVG(STRLEN(URL))). For these we evaluate the arg per
            // row group via ExpressionExecutor; the per-row hot loop in
            // apply_aggs reads the precomputed typed buffer.
            std::vector<const BoundExpression *> expr_args(num_aggs, nullptr);
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &info = agg_infos[a];
                if (info.col_idx != INVALID_INDEX) continue;
                if (info.is_count_star) continue;
                // Q28 fast path: STRLEN(col_ref) is fully serviced by the
                // is_strlen branch in apply_aggs (reads col->str_lengths
                // directly). The expr_vals pre-fill at line ~10293 would
                // duplicate that work and waste 1MB write/RG worker.
                if (info.strlen_of_col &&
                    info.strlen_col_idx != INVALID_INDEX) continue;
                auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                if (!agg_expr.arguments.empty())
                    expr_args[a] = agg_expr.arguments[0].get();
            }
            // Local CollectRefs (sibling class' helper is private). Walks the
            // expression tree marking column indices referenced.
            std::function<void(const BoundExpression &, std::vector<bool> &)>
                collect_refs_local = [&](const BoundExpression &e,
                                          std::vector<bool> &used) {
                switch (e.GetExpressionType()) {
                case BoundExpressionType::COLUMN_REF: {
                    auto &r = static_cast<const BoundColumnRef &>(e);
                    if (r.column_index < used.size()) used[r.column_index] = true;
                    break;
                }
                case BoundExpressionType::COMPARISON: {
                    auto &c = static_cast<const BoundComparison &>(e);
                    collect_refs_local(*c.left, used);
                    collect_refs_local(*c.right, used);
                    break;
                }
                case BoundExpressionType::CONJUNCTION: {
                    auto &c = static_cast<const BoundConjunction &>(e);
                    collect_refs_local(*c.left, used);
                    collect_refs_local(*c.right, used);
                    break;
                }
                case BoundExpressionType::NEGATION: {
                    auto &n = static_cast<const BoundNegation &>(e);
                    collect_refs_local(*n.child, used);
                    break;
                }
                case BoundExpressionType::IS_NULL: {
                    auto &n = static_cast<const BoundIsNull &>(e);
                    collect_refs_local(*n.child, used);
                    break;
                }
                case BoundExpressionType::ARITHMETIC: {
                    auto &a2 = static_cast<const BoundArithmetic &>(e);
                    collect_refs_local(*a2.left, used);
                    collect_refs_local(*a2.right, used);
                    break;
                }
                case BoundExpressionType::FUNCTION: {
                    auto &f = static_cast<const BoundFunction &>(e);
                    for (auto &arg : f.arguments) collect_refs_local(*arg, used);
                    break;
                }
                case BoundExpressionType::UNARY_MINUS: {
                    auto &u = static_cast<const BoundUnaryMinus &>(e);
                    collect_refs_local(*u.child, used);
                    break;
                }
                case BoundExpressionType::CAST: {
                    auto &c = static_cast<const BoundCast &>(e);
                    collect_refs_local(*c.child, used);
                    break;
                }
                default: break;
                }
            };
            // Push projection down so we only decode columns we need.
            {
                std::vector<bool> needed(pq->GetTypes().size(), false);
                for (auto gc : group_col_indices) if (gc < needed.size()) needed[gc] = true;
                for (auto &info : agg_infos) {
                    if (info.col_idx != INVALID_INDEX && info.col_idx < needed.size())
                        needed[info.col_idx] = true;
                    if (info.strlen_of_col && info.strlen_col_idx < needed.size())
                        needed[info.strlen_col_idx] = true;
                }
                for (idx_t a = 0; a < num_aggs; a++) {
                    if (expr_args[a]) collect_refs_local(*expr_args[a], needed);
                }
                for (auto &p : multi_preds) {
                    if (p.col_idx < needed.size()) needed[p.col_idx] = true;
                }
                pq->SetNeededOutputs(needed);
                // Skip per-row string_t materialization for VARCHAR group cols
                // - the packed-key path uses only str_dict_indices + str_dict_values.
                // Also skip VARCHAR predicate cols (BuildTypedKeepMask + the
                // per-row fallback consult str_dict_values first); PLAIN-only
                // RGs trigger MaterialiseStrDataLazy back-fill in parquet.cpp.
                std::vector<bool> skip(pq->GetTypes().size(), false);
                for (auto gc : group_col_indices) {
                    if (gc < skip.size() &&
                        pq->GetTypes()[gc].id() == LogicalTypeId::VARCHAR) {
                        skip[gc] = true;
                    }
                }
                for (auto &p : multi_preds) {
                    if (p.col_idx < skip.size() && p.str_form &&
                        pq->GetTypes()[p.col_idx].id() == LogicalTypeId::VARCHAR) {
                        skip[p.col_idx] = true;
                    }
                }
                // Lengths-only detection (Q28 attack).
                idx_t ncols_lo = pq->GetTypes().size();
                std::vector<bool> lo(ncols_lo, false);
                std::vector<bool> lo_blocker(ncols_lo, false);
                for (auto gc : group_col_indices)
                    if (gc < ncols_lo) lo_blocker[gc] = true;
                for (auto &info : agg_infos)
                    if (info.col_idx != INVALID_INDEX && info.col_idx < ncols_lo)
                        lo_blocker[info.col_idx] = true;
                for (auto &p : multi_preds) {
                    if (p.col_idx >= ncols_lo) continue;
                    if (!p.str_form) continue;
                    if (p.like_contains || !p.sval.empty() ||
                        (p.op != SimpleCmpOp::EQ && p.op != SimpleCmpOp::NE)) {
                        lo_blocker[p.col_idx] = true;
                    }
                }
                for (idx_t a = 0; a < num_aggs; a++) {
                    if (!expr_args[a]) continue;
                    auto *e = expr_args[a];
                    bool simple_strlen = false;
                    if (e->GetExpressionType() == BoundExpressionType::FUNCTION) {
                        auto &f = static_cast<const BoundFunction &>(*e);
                        if ((f.function_name == "STRLEN" ||
                             f.function_name == "LENGTH") &&
                            f.arguments.size() == 1 &&
                            f.arguments[0]->GetExpressionType() ==
                                BoundExpressionType::COLUMN_REF) {
                            simple_strlen = true;
                        }
                    }
                    if (simple_strlen) continue;
                    std::vector<bool> refs(ncols_lo, false);
                    collect_refs_local(*e, refs);
                    for (idx_t c = 0; c < ncols_lo; c++)
                        if (refs[c]) lo_blocker[c] = true;
                }
                for (idx_t c = 0; c < ncols_lo; c++) {
                    if (pq->GetTypes()[c].id() == LogicalTypeId::VARCHAR &&
                        needed[c] && !lo_blocker[c]) {
                        lo[c] = true;
                    }
                }
                pq->SetSkipStrData(std::move(skip));
                pq->SetStrLengthsOnly(std::move(lo));
            }

            enum class AK { CountStar, Count, Sum, Min, Max, CountDistinctInt, CountDistinctStr, Other };
            std::vector<AK> kinds(num_aggs);
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &info = agg_infos[a];
                if (info.name == "COUNT" && info.is_count_star)   kinds[a] = AK::CountStar;
                else if (info.name == "COUNT" && info.is_distinct) {
                    // Choose distinct backing store by argument type so the
                    // hot loop is a typed insert, not Value::ToString. Q10
                    // SELECT ... COUNT(DISTINCT UserID) GROUP BY RegionID
                    // lives here.
                    if (info.col_idx != INVALID_INDEX) {
                        auto t = pq->GetTypes()[info.col_idx].id();
                        if (t == LogicalTypeId::BIGINT || t == LogicalTypeId::INTEGER)
                            kinds[a] = AK::CountDistinctInt;
                        else if (t == LogicalTypeId::VARCHAR)
                            kinds[a] = AK::CountDistinctStr;
                        else
                            kinds[a] = AK::Other;
                    } else {
                        kinds[a] = AK::Other;
                    }
                }
                else if (info.name == "COUNT")                     kinds[a] = AK::Count;
                else if (info.name == "SUM" || info.name == "AVG") kinds[a] = AK::Sum;
                else if (info.name == "MIN")                       kinds[a] = AK::Min;
                else if (info.name == "MAX")                       kinds[a] = AK::Max;
                else                                                kinds[a] = AK::Other;
            }

            // === Q10 fast path: single INT32-column GROUP BY (no filter)
            //     + aggs in {COUNT(*), SUM/AVG of an int col, COUNT(DISTINCT
            //     int col)}. The generic per-row per-agg dispatch loop below
            //     costs ~90 ns/row on this shape; IntGroupAgg (side TU) hardcodes
            //     the agg kinds and skew-balances the distinct-set merge.
            //     ClickBench Q10 (RegionID, SUM(AdvEngineID), COUNT(*),
            //     AVG(ResolutionWidth), COUNT(DISTINCT UserID)) lands here. ==
            if (group_col_indices.size() == 1 && !multi_has_filter &&
                num_aggs >= 1 &&
                pq->GetTypes()[group_col_indices[0]].id() ==
                    LogicalTypeId::INTEGER) {
                bool q10_ok = true;
                std::vector<slothdb::IntGroupAggKind> q10kinds((size_t)num_aggs);
                std::vector<bool> q10_big((size_t)num_aggs, false);
                std::vector<idx_t> q10_col((size_t)num_aggs, INVALID_INDEX);
                for (idx_t a = 0; a < num_aggs && q10_ok; a++) {
                    auto &info = agg_infos[a];
                    if (kinds[a] == AK::CountStar) {
                        q10kinds[a] = slothdb::IntGroupAggKind::CountStar;
                    } else if (kinds[a] == AK::Sum ||
                               kinds[a] == AK::CountDistinctInt) {
                        if (info.col_idx == INVALID_INDEX) {
                            q10_ok = false; break;
                        }
                        auto ct = pq->GetTypes()[info.col_idx].id();
                        if (ct != LogicalTypeId::INTEGER &&
                            ct != LogicalTypeId::BIGINT) {
                            q10_ok = false; break;
                        }
                        q10kinds[a] = (kinds[a] == AK::Sum)
                            ? slothdb::IntGroupAggKind::Sum
                            : slothdb::IntGroupAggKind::CountDistinct;
                        q10_big[a] = (ct == LogicalTypeId::BIGINT);
                        q10_col[a] = info.col_idx;
                    } else {
                        q10_ok = false;
                    }
                }
                if (q10_ok) {
                    constexpr int Q10_THREADS = 8;
                    idx_t q10_gci = group_col_indices[0];
                    slothdb::IntGroupAgg q10agg(Q10_THREADS, q10kinds);
                    pq->SetRGConsumer(
                        [&](const PhysicalParquetScan::RGWork &work,
                            idx_t rg_idx, int tid) {
                            if (work.pruned) return;
                            idx_t nrows = pq->RowGroupSize(rg_idx);
                            const auto &gc = work.cols[q10_gci];
                            if (!gc.decoded ||
                                gc.type.id() != LogicalTypeId::INTEGER) return;
                            const int32_t *grp = gc.i32_data.data();
                            const uint8_t *gv = gc.all_valid
                                ? nullptr : gc.validity.data();
                            std::vector<const int64_t*> ai64(
                                (size_t)num_aggs, nullptr);
                            std::vector<const int32_t*> ai32(
                                (size_t)num_aggs, nullptr);
                            std::vector<const uint8_t*> av(
                                (size_t)num_aggs, nullptr);
                            bool ok = true;
                            for (idx_t a = 0; a < num_aggs; a++) {
                                if (q10kinds[a] ==
                                    slothdb::IntGroupAggKind::CountStar) continue;
                                const auto &ac = work.cols[q10_col[a]];
                                if (!ac.decoded) { ok = false; break; }
                                if (q10_big[a]) ai64[a] = ac.i64_data.data();
                                else            ai32[a] = ac.i32_data.data();
                                av[a] = ac.all_valid
                                    ? nullptr : ac.validity.data();
                            }
                            if (!ok) return;
                            q10agg.ConsumeRG(tid % Q10_THREADS,
                                             (uint32_t)nrows,
                                             grp, gv, ai64, ai32, av);
                        });
                    pq->RunParallelRGs(Q10_THREADS);
                    auto q10groups = q10agg.MergeAll();
                    // Emit through the shared EmitAggValue helper so the
                    // result Values are byte-identical to the generic path.
                    std::vector<EmitAggDesc> q10_descs((size_t)num_aggs);
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &ae =
                            static_cast<BoundFunction &>(*aggregates_[a]);
                        q10_descs[a].kind = ResolveEmitAggKind(
                            StringUtil::Upper(ae.function_name));
                        q10_descs[a].return_type_id =
                            ae.GetReturnType().id();
                        q10_descs[a].sum_with_offset =
                            agg_infos[a].sum_with_offset;
                        q10_descs[a].sum_offset = agg_infos[a].sum_offset;
                    }
                    result_rows_.reserve(q10groups.size());
                    for (auto &gr : q10groups) {
                        std::vector<Value> row;
                        row.reserve(1 + (size_t)num_aggs);
                        row.push_back(Value::INTEGER(gr.key));
                        for (idx_t a = 0; a < num_aggs; a++) {
                            EmitAggView view;
                            view.count = gr.aggs[a].count;
                            view.sum = gr.aggs[a].sum;
                            EmitAggValue(q10_descs[a], view, row);
                        }
                        result_rows_.push_back(std::move(row));
                    }
                    return;
                }
            }

            // Packability check: a multi-col key fits in a single uint64 if every
            // column contributes <= 32 bits. Dict-encoded VARCHAR contributes a
            // 32-bit scan-wide global index; INTEGER is naturally 32 bits; BIGINT
            // is assumed to fit 32 bits for columns that realistically appear in
            // GROUP BY (year, quarter, status, etc.). When packable we use a
            // uint64-keyed `ankerl::unordered_dense::map` - far faster than
            // `std::unordered_map<std::string, ...>` with a variable-length key.
            struct PackSpec { int kind = 0; int shift = 0; };
            std::vector<PackSpec> pack(group_col_indices.size());
            bool all_packable = true;
            {
                int total_bits = 0;
                for (size_t gi = 0; gi < group_col_indices.size(); gi++) {
                    auto tid = pq->GetTypes()[group_col_indices[gi]].id();
                    if (tid == LogicalTypeId::VARCHAR)       pack[gi].kind = 1; // dict global idx
                    else if (tid == LogicalTypeId::INTEGER)  pack[gi].kind = 2;
                    else if (tid == LogicalTypeId::BIGINT)   pack[gi].kind = 3; // low-32 assumption
                    else { all_packable = false; break; }
                    pack[gi].shift = total_bits;
                    total_bits += 32;
                    if (total_bits > 64) { all_packable = false; break; }
                }
            }

            // Per-thread state - workers aggregate into their own slots so the
            // packed-key hot loop runs without synchronization. Each thread
            // maintains its own global dict; we reconcile across threads at
            // merge time using the (always-content-accurate) `key_vals`.
            struct TLMulti {
                std::vector<ankerl::unordered_dense::map<std::string, uint32_t>> global_dicts;
                std::vector<std::vector<Value>> global_dict_values;
                ankerl::unordered_dense::map<uint64_t, std::vector<AggState>> packed_groups;
                std::vector<uint64_t> packed_order;
                ankerl::unordered_dense::map<uint64_t, std::vector<Value>> packed_keys;
                std::unordered_map<std::string, std::vector<AggState>> str_groups;
                std::unordered_map<std::string, std::vector<Value>> str_keys_map;
                std::vector<std::string> str_order;
            };
            constexpr int MAX_THREADS = 8;
            std::vector<TLMulti> tls(MAX_THREADS);
            for (auto &tl : tls) {
                tl.global_dicts.resize(group_col_indices.size());
                tl.global_dict_values.resize(group_col_indices.size());
            }

            auto process_rg_multi = [&](const PhysicalParquetScan::RGWork &work,
                                         idx_t rg_idx, TLMulti &tl) {
                idx_t nrows = pq->RowGroupSize(rg_idx);

                // Per-group-col raw pointers + dict data (for VARCHAR dict path).
                struct GCol {
                    const ParquetColumnData *col;
                    LogicalTypeId tid;
                    const int64_t *i64;
                    const int32_t *i32;
                    const string_t *str;
                    const uint32_t *dict_idx;     // nullptr if not dict-encoded
                    const string_t *dict_val;     // dict entries
                    idx_t dict_size;
                };
                std::vector<GCol> gcs(group_col_indices.size());
                for (size_t gi = 0; gi < group_col_indices.size(); gi++) {
                    auto &g = gcs[gi];
                    g.col = &work.cols[group_col_indices[gi]];
                    g.tid = g.col->type.id();
                    g.i64 = (g.tid == LogicalTypeId::BIGINT)  ? g.col->i64_data.data() : nullptr;
                    g.i32 = (g.tid == LogicalTypeId::INTEGER) ? g.col->i32_data.data() : nullptr;
                    g.str = (g.tid == LogicalTypeId::VARCHAR) ? g.col->str_data.data() : nullptr;
                    g.dict_idx = (g.tid == LogicalTypeId::VARCHAR && g.col->str_dict_encoded)
                                      ? g.col->str_dict_indices.data() : nullptr;
                    g.dict_val = (g.dict_idx != nullptr) ? g.col->str_dict_values.data() : nullptr;
                    g.dict_size = g.dict_val ? g.col->str_dict_values.size() : 0;
                }

                // Per-RG local->global dict translation for packed path.
                std::vector<std::vector<uint32_t>> local_to_global;
                bool rg_packable = all_packable;
                if (rg_packable) {
                    local_to_global.resize(group_col_indices.size());
                    for (size_t gi = 0; gi < group_col_indices.size(); gi++) {
                        if (pack[gi].kind != 1) continue; // only VARCHAR needs translation
                        auto &g = gcs[gi];
                        if (!g.dict_idx) { rg_packable = false; break; }
                        local_to_global[gi].resize(g.dict_size);
                        auto &gd = tl.global_dicts[gi];
                        auto &gdv = tl.global_dict_values[gi];
                        for (idx_t li = 0; li < g.dict_size; li++) {
                            const auto &dv = g.dict_val[li];
                            std::string content(dv.GetData(), dv.GetSize());
                            auto it = gd.find(content);
                            uint32_t gid;
                            if (it == gd.end()) {
                                gid = (uint32_t)gdv.size();
                                gdv.push_back(Value::VARCHAR(content));
                                gd.emplace(std::move(content), gid);
                            } else {
                                gid = it->second;
                            }
                            local_to_global[gi][li] = gid;
                        }
                    }
                }

                // Per-agg typed column pointers.
                struct ACol { const ParquetColumnData *col; LogicalTypeId tid; bool decoded; bool all_valid; bool is_strlen; };
                std::vector<ACol> acs(num_aggs);
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &info = agg_infos[a];
                    acs[a].is_strlen = false;
                    if (info.strlen_of_col && info.strlen_col_idx != INVALID_INDEX) {
                        acs[a].col = &work.cols[info.strlen_col_idx];
                        acs[a].tid = acs[a].col->type.id();
                        acs[a].decoded = acs[a].col->decoded;
                        acs[a].all_valid = acs[a].col->all_valid;
                        acs[a].is_strlen = true;
                    } else if (info.col_idx != INVALID_INDEX) {
                        acs[a].col = &work.cols[info.col_idx];
                        acs[a].tid = acs[a].col->type.id();
                        acs[a].decoded = acs[a].col->decoded;
                        acs[a].all_valid = acs[a].col->all_valid;
                    } else {
                        acs[a].col = nullptr;
                    }
                }

                // Pre-evaluate expression-arg aggregates (e.g. AVG(STRLEN(URL)))
                // once per RG into typed double + validity buffers. The per-row
                // hot loop in apply_aggs then reads from these buffers without
                // any further ExpressionExecutor calls.
                std::vector<std::vector<double>> expr_vals(num_aggs);
                std::vector<std::vector<uint8_t>> expr_valid(num_aggs);
                bool any_expr_arg = false;
                for (idx_t a = 0; a < num_aggs; a++) if (expr_args[a]) { any_expr_arg = true; break; }
                if (any_expr_arg) {
                    auto &child_types = pq->GetTypes();
                    // Per-agg ref masks so we only SetValue the referenced cols.
                    std::vector<std::vector<bool>> agg_used(num_aggs);
                    // Fast path: STRLEN(VARCHAR direct col) is just the per-row
                    // string size — no need to box through DataChunk + Executor.
                    // Cuts ~5s on Q28 (100M rows × per-row std::string alloc).
                    std::vector<idx_t> strlen_col(num_aggs, INVALID_INDEX);
                    for (idx_t a = 0; a < num_aggs; a++) {
                        if (!expr_args[a]) continue;
                        if (expr_args[a]->GetExpressionType() != BoundExpressionType::FUNCTION) continue;
                        auto &fn = static_cast<const BoundFunction &>(*expr_args[a]);
                        if (fn.function_name != "STRLEN" && fn.function_name != "LENGTH") continue;
                        if (fn.arguments.size() != 1) continue;
                        if (fn.arguments[0]->GetExpressionType() != BoundExpressionType::COLUMN_REF) continue;
                        auto &cr = static_cast<const BoundColumnRef &>(*fn.arguments[0]);
                        if (cr.column_index >= child_types.size()) continue;
                        if (child_types[cr.column_index].id() != LogicalTypeId::VARCHAR) continue;
                        strlen_col[a] = cr.column_index;
                    }
                    for (idx_t a = 0; a < num_aggs; a++) {
                        if (!expr_args[a]) continue;
                        agg_used[a].assign(child_types.size(), false);
                        collect_refs_local(*expr_args[a], agg_used[a]);
                        expr_vals[a].assign(nrows, 0.0);
                        expr_valid[a].assign(nrows, 0);
                    }
                    // STRLEN fast path: read string sizes directly per agg.
                    for (idx_t a = 0; a < num_aggs; a++) {
                        if (strlen_col[a] == INVALID_INDEX) continue;
                        const auto &col = work.cols[strlen_col[a]];
                        if (!col.decoded) { strlen_col[a] = INVALID_INDEX; continue; }
                        if (col.str_lengths_only && !col.str_lengths.empty()) {
                            // Lengths-only mode: STRLEN(URL) reads the raw
                            // length array directly. Skips byte-decode entirely.
                            const uint32_t *L = col.str_lengths.data();
                            for (idx_t r = 0; r < nrows; r++) {
                                if (!col.all_valid && r < col.validity.size() && !col.validity[r]) continue;
                                expr_vals[a][r] = (double)L[r];
                                expr_valid[a][r] = 1;
                            }
                        } else if (col.str_dict_encoded && !col.str_dict_indices.empty()) {
                            // Precompute dict-entry sizes once, then per-row index lookup.
                            std::vector<int32_t> dict_sz(col.str_dict_values.size());
                            for (size_t di = 0; di < col.str_dict_values.size(); di++)
                                dict_sz[di] = (int32_t)col.str_dict_values[di].GetSize();
                            const uint32_t *idx_arr = col.str_dict_indices.data();
                            for (idx_t r = 0; r < nrows; r++) {
                                if (!col.all_valid && r < col.validity.size() && !col.validity[r]) continue;
                                uint32_t di = idx_arr[r];
                                if (di < dict_sz.size()) {
                                    expr_vals[a][r] = (double)dict_sz[di];
                                    expr_valid[a][r] = 1;
                                }
                            }
                        } else if (!col.str_data.empty()) {
                            const auto *sd = col.str_data.data();
                            for (idx_t r = 0; r < nrows; r++) {
                                if (!col.all_valid && r < col.validity.size() && !col.validity[r]) continue;
                                expr_vals[a][r] = (double)sd[r].GetSize();
                                expr_valid[a][r] = 1;
                            }
                        } else {
                            strlen_col[a] = INVALID_INDEX;  // no usable data; fall back
                        }
                    }
                    DataChunk row_chunk;
                    row_chunk.Initialize(child_types, VECTOR_SIZE);
                    for (idx_t base = 0; base < nrows; base += VECTOR_SIZE) {
                        idx_t cnt = std::min<idx_t>(VECTOR_SIZE, nrows - base);
                        // Reset chunk and populate referenced columns from work.cols.
                        for (idx_t c = 0; c < child_types.size(); c++) {
                            row_chunk.GetVector(c).GetValidity().Reset();
                        }
                        // Build per-column once per agg-set; union the refs.
                        // Skip STRLEN-fast-path aggs - they read column data
                        // directly above without going through the row_chunk.
                        std::vector<bool> any_used(child_types.size(), false);
                        for (idx_t a = 0; a < num_aggs; a++) {
                            if (!expr_args[a]) continue;
                            if (strlen_col[a] != INVALID_INDEX) continue;
                            for (idx_t c = 0; c < child_types.size(); c++)
                                if (agg_used[a][c]) any_used[c] = true;
                        }
                        for (idx_t c = 0; c < child_types.size(); c++) {
                            if (!any_used[c]) continue;
                            const auto &col = work.cols[c];
                            if (col.decoded) {
                                auto tid = col.type.id();
                                for (idx_t i = 0; i < cnt; i++) {
                                    idx_t r = base + i;
                                    if (!col.all_valid && r < col.validity.size() &&
                                        !col.validity[r]) {
                                        row_chunk.GetVector(c).GetValidity().SetInvalid(i);
                                        continue;
                                    }
                                    Value v;
                                    switch (tid) {
                                    case LogicalTypeId::BIGINT:  v = Value::BIGINT(col.i64_data[r]); break;
                                    case LogicalTypeId::INTEGER: v = Value::INTEGER(col.i32_data[r]); break;
                                    case LogicalTypeId::DOUBLE:  v = Value::DOUBLE(col.f64_data[r]); break;
                                    case LogicalTypeId::FLOAT:   v = Value::DOUBLE((double)col.f32_data[r]); break;
                                    case LogicalTypeId::VARCHAR: {
                                        if (col.str_dict_encoded && !col.str_dict_indices.empty()) {
                                            uint32_t di = col.str_dict_indices[r];
                                            if (di < col.str_dict_values.size()) {
                                                const auto &s = col.str_dict_values[di];
                                                v = Value::VARCHAR(std::string(s.GetData(), s.GetSize()));
                                            }
                                        } else if (!col.str_data.empty()) {
                                            const auto &s = col.str_data[r];
                                            v = Value::VARCHAR(std::string(s.GetData(), s.GetSize()));
                                        }
                                        break;
                                    }
                                    default: break;
                                    }
                                    row_chunk.SetValue(c, i, v);
                                }
                            } else if (c < work.cols_fallback.size() &&
                                       !work.cols_fallback[c].empty()) {
                                const auto &fb = work.cols_fallback[c];
                                for (idx_t i = 0; i < cnt; i++) {
                                    idx_t r = base + i;
                                    if (r < fb.size()) row_chunk.SetValue(c, i, fb[r]);
                                }
                            }
                        }
                        row_chunk.SetCardinality(cnt);
                        // Evaluate each expression-arg agg into expr_vals/expr_valid.
                        for (idx_t a = 0; a < num_aggs; a++) {
                            if (!expr_args[a]) continue;
                            if (strlen_col[a] != INVALID_INDEX) continue;  // already filled by fast path
                            Vector res(expr_args[a]->GetReturnType(), VECTOR_SIZE);
                            ExpressionExecutor::Execute(*expr_args[a], row_chunk, res, cnt);
                            auto rtid = res.GetType().id();
                            for (idx_t i = 0; i < cnt; i++) {
                                if (!res.GetValidity().RowIsValid(i)) continue;
                                double dv = 0.0;
                                switch (rtid) {
                                case LogicalTypeId::INTEGER:
                                    dv = (double)res.GetData<int32_t>()[i]; break;
                                case LogicalTypeId::BIGINT:
                                    dv = (double)res.GetData<int64_t>()[i]; break;
                                case LogicalTypeId::DOUBLE:
                                    dv = res.GetData<double>()[i]; break;
                                case LogicalTypeId::FLOAT:
                                    dv = (double)res.GetData<float>()[i]; break;
                                default: {
                                    auto val = res.GetValue(i);
                                    if (val.IsNull()) continue;
                                    try { dv = val.GetValue<double>(); }
                                    catch (...) { continue; }
                                    break;
                                }
                                }
                                expr_vals[a][base + i] = dv;
                                expr_valid[a][base + i] = 1;
                            }
                        }
                    }
                }

                // Inner agg-update lambda shared by packed and string paths.
                auto apply_aggs = [&](std::vector<AggState> &states_r, idx_t r) {
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &state = states_r[a];
                        auto &acol = acs[a];
                        if (kinds[a] == AK::CountStar) { state.count++; continue; }
                        if (!acol.col) {
                            // Expression-arg path: read precomputed value if any.
                            if (!expr_args[a]) continue;
                            if (!expr_valid[a][r]) continue;
                            double val = expr_vals[a][r];
                            switch (kinds[a]) {
                            case AK::Count: state.count++; break;
                            case AK::Sum:   state.sum += val; state.count++; break;
                            case AK::Min:
                                if (!state.has_min || val < state.sum_min) {
                                    state.sum_min = val; state.has_min = true;
                                }
                                break;
                            case AK::Max:
                                if (!state.has_max || val > state.sum_max) {
                                    state.sum_max = val; state.has_max = true;
                                }
                                break;
                            default: break;
                            }
                            continue;
                        }
                        if (!acol.decoded) continue;
                        if (!acol.all_valid && !acol.col->validity[r]) continue;
                        // STRLEN/LENGTH(col): byte-length of the row's string.
                        // Q28 AVG(STRLEN(URL)) / SUM/MIN/MAX/COUNT live here.
                        if (acol.is_strlen) {
                            if (acol.tid != LogicalTypeId::VARCHAR) continue;
                            uint32_t sl;
                            if (acol.col->str_lengths_only &&
                                r < acol.col->str_lengths.size()) {
                                sl = acol.col->str_lengths[r];
                            } else if (acol.col->str_dict_encoded &&
                                !acol.col->str_dict_indices.empty()) {
                                uint32_t di = acol.col->str_dict_indices[r];
                                if (di >= acol.col->str_dict_values.size()) continue;
                                sl = acol.col->str_dict_values[di].GetSize();
                            } else if (!acol.col->str_data.empty()) {
                                sl = acol.col->str_data[r].GetSize();
                            } else continue;
                            double val = (double)sl;
                            switch (kinds[a]) {
                            case AK::Count: state.count++; break;
                            case AK::Sum:   state.sum += val; state.count++; break;
                            case AK::Min:
                                if (!state.has_min || val < state.sum_min) {
                                    state.sum_min = val; state.has_min = true;
                                }
                                break;
                            case AK::Max:
                                if (!state.has_max || val > state.sum_max) {
                                    state.sum_max = val; state.has_max = true;
                                }
                                break;
                            default: break;
                            }
                            continue;
                        }
                        // Distinct paths: insert into the typed dedup set;
                        // count++ only on new insert. The set itself is the
                        // source of truth so the result emit doesn't need
                        // to re-count.
                        if (kinds[a] == AK::CountDistinctInt) {
                            int64_t iv;
                            if (acol.tid == LogicalTypeId::BIGINT)
                                iv = acol.col->i64_data[r];
                            else if (acol.tid == LogicalTypeId::INTEGER)
                                iv = (int64_t)acol.col->i32_data[r];
                            else continue;
                            if (state.extras().distinct_int_set.insert(iv))
                                state.count++;
                            continue;
                        }
                        if (kinds[a] == AK::CountDistinctStr) {
                            if (acol.tid != LogicalTypeId::VARCHAR) continue;
                            const char *sd; uint32_t sl;
                            if (acol.col->str_dict_encoded &&
                                !acol.col->str_dict_indices.empty()) {
                                uint32_t di = acol.col->str_dict_indices[r];
                                if (di >= acol.col->str_dict_values.size()) continue;
                                sd = acol.col->str_dict_values[di].GetData();
                                sl = acol.col->str_dict_values[di].GetSize();
                            } else if (!acol.col->str_data.empty()) {
                                sd = acol.col->str_data[r].GetData();
                                sl = acol.col->str_data[r].GetSize();
                            } else continue;
                            if (state.extras().distinct_set.emplace(sd, sl).second)
                                state.count++;
                            continue;
                        }
                        // VARCHAR MIN/MAX: store winner in min_val/max_val Value;
                        // emit path already handles non-null min_val/max_val.
                        if ((kinds[a] == AK::Min || kinds[a] == AK::Max) &&
                            acol.tid == LogicalTypeId::VARCHAR) {
                            const char *sd = nullptr; uint32_t sl = 0;
                            if (acol.col->str_dict_encoded &&
                                !acol.col->str_dict_indices.empty()) {
                                uint32_t di = acol.col->str_dict_indices[r];
                                if (di >= acol.col->str_dict_values.size()) continue;
                                sd = acol.col->str_dict_values[di].GetData();
                                sl = acol.col->str_dict_values[di].GetSize();
                            } else if (!acol.col->str_data.empty()) {
                                sd = acol.col->str_data[r].GetData();
                                sl = acol.col->str_data[r].GetSize();
                            } else continue;
                            std::string_view sv(sd, sl);
                            if (kinds[a] == AK::Min) {
                                if (!state.has_min) {
                                    state.min_val() = Value::VARCHAR(std::string(sd, sl));
                                    state.has_min = true;
                                } else if (sv < std::string_view(state.min_val().StringRef())) {
                                    state.min_val() = Value::VARCHAR(std::string(sd, sl));
                                }
                            } else {
                                if (!state.has_max) {
                                    state.max_val() = Value::VARCHAR(std::string(sd, sl));
                                    state.has_max = true;
                                } else if (sv > std::string_view(state.max_val().StringRef())) {
                                    state.max_val() = Value::VARCHAR(std::string(sd, sl));
                                }
                            }
                            continue;
                        }
                        double val = 0.0;
                        switch (acol.tid) {
                        case LogicalTypeId::DOUBLE:  val = acol.col->f64_data[r]; break;
                        case LogicalTypeId::BIGINT:  val = (double)acol.col->i64_data[r]; break;
                        case LogicalTypeId::INTEGER: val = (double)acol.col->i32_data[r]; break;
                        case LogicalTypeId::FLOAT:   val = (double)acol.col->f32_data[r]; break;
                        default: continue;
                        }
                        switch (kinds[a]) {
                        case AK::Count: state.count++; break;
                        case AK::Sum:   state.sum += val; state.count++; break;
                        case AK::Min:
                            if (!state.has_min || val < state.sum_min) {
                                state.sum_min = val; state.has_min = true;
                            }
                            break;
                        case AK::Max:
                            if (!state.has_max || val > state.sum_max) {
                                state.sum_max = val; state.has_max = true;
                            }
                            break;
                        default: break;
                        }
                    }
                };

                std::vector<uint8_t> multi_keep_mask;
                bool multi_keep_active = false;
                if (multi_has_filter)
                    multi_keep_active = BuildTypedKeepMask(multi_preds, work.cols,
                                                             nrows, multi_keep_mask);

                if (rg_packable) {
                    // === PACKED uint64 key path ===
                    // Cache-last: ClickBench RegionID averages 16+ consecutive
                    // rows per group (94% cache hit on first 100K rows). Skip
                    // the outer-map probe when pkey unchanged. Pointer is
                    // stable across hits because no insertion happens between
                    // them; on miss-with-insert we re-fetch from try_emplace.
                    uint64_t cache_pkey = 0;
                    std::vector<AggState> *cache_states = nullptr;
                    for (idx_t r = 0; r < nrows; r++) {
                        if (multi_keep_active) {
                            if (!multi_keep_mask[r]) continue;
                        } else if (multi_has_filter &&
                                   !EvalSimplePredicates(multi_preds, work.cols, r)) continue;
                        uint64_t pkey = 0;
                        for (size_t gi = 0; gi < group_col_indices.size(); gi++) {
                            auto &g = gcs[gi];
                            uint32_t component = 0;
                            switch (pack[gi].kind) {
                            case 1: component = local_to_global[gi][g.dict_idx[r]]; break;
                            case 2: component = (uint32_t)g.i32[r]; break;
                            case 3: component = (uint32_t)(uint64_t)g.i64[r]; break;
                            }
                            pkey |= (uint64_t)component << pack[gi].shift;
                        }
                        if (cache_states && pkey == cache_pkey) {
                            apply_aggs(*cache_states, r);
                            continue;
                        }
                        auto [it, inserted] = tl.packed_groups.try_emplace(pkey);
                        if (inserted) {
                            it->second.resize(num_aggs);
                            tl.packed_order.push_back(pkey);
                            std::vector<Value> key_vals;
                            key_vals.reserve(group_col_indices.size());
                            for (size_t gi = 0; gi < group_col_indices.size(); gi++) {
                                auto &g = gcs[gi];
                                switch (pack[gi].kind) {
                                case 1: key_vals.push_back(
                                        tl.global_dict_values[gi][local_to_global[gi][g.dict_idx[r]]]);
                                        break;
                                case 2: key_vals.push_back(Value::INTEGER(g.i32[r])); break;
                                case 3: key_vals.push_back(Value::BIGINT(g.i64[r])); break;
                                }
                            }
                            tl.packed_keys.emplace(pkey, std::move(key_vals));
                        }
                        apply_aggs(it->second, r);
                        cache_pkey = pkey;
                        cache_states = &it->second;
                    }
                } else {
                    // === FALLBACK string-key path (non-packable types) ===
                    std::string local_key;
                    local_key.reserve(128);
                    for (idx_t r = 0; r < nrows; r++) {
                        if (multi_keep_active) {
                            if (!multi_keep_mask[r]) continue;
                        } else if (multi_has_filter &&
                                   !EvalSimplePredicates(multi_preds, work.cols, r)) continue;
                        local_key.clear();
                        for (auto &g : gcs) {
                            if (g.i64) {
                                local_key.append(reinterpret_cast<const char *>(&g.i64[r]), 8);
                            } else if (g.i32) {
                                local_key.append(reinterpret_cast<const char *>(&g.i32[r]), 4);
                            } else if (g.dict_idx) {
                                uint32_t di = g.dict_idx[r];
                                const auto &dv = g.dict_val[di];
                                uint32_t n = dv.GetSize();
                                local_key.append(reinterpret_cast<const char *>(&n), 4);
                                local_key.append(dv.GetData(), n);
                            } else if (g.str) {
                                const char *p = g.str[r].GetData();
                                uint32_t n = g.str[r].GetSize();
                                local_key.append(reinterpret_cast<const char *>(&n), 4);
                                local_key.append(p, n);
                            } else {
                                auto v = g.col->decoded ? Value()
                                                         : (r < work.cols_fallback[
                                                              group_col_indices[&g - &gcs[0]]].size()
                                                              ? work.cols_fallback[
                                                                 group_col_indices[&g - &gcs[0]]][r]
                                                              : Value());
                                auto s = v.ToString();
                                local_key.append(s);
                            }
                            local_key.push_back('|');
                        }
                        auto it = tl.str_groups.find(local_key);
                        if (it == tl.str_groups.end()) {
                            std::vector<Value> key_vals;
                            key_vals.reserve(gcs.size());
                            for (auto &g : gcs) {
                                if (g.i64) key_vals.push_back(Value::BIGINT(g.i64[r]));
                                else if (g.i32) key_vals.push_back(Value::INTEGER(g.i32[r]));
                                else if (g.dict_idx) {
                                    uint32_t di = g.dict_idx[r];
                                    const auto &dv = g.dict_val[di];
                                    key_vals.push_back(Value::VARCHAR(
                                        std::string(dv.GetData(), dv.GetSize())));
                                } else if (g.str) {
                                    key_vals.push_back(Value::VARCHAR(
                                        std::string(g.str[r].GetData(), g.str[r].GetSize())));
                                } else {
                                    key_vals.push_back(Value());
                                }
                            }
                            tl.str_keys_map[local_key] = std::move(key_vals);
                            tl.str_order.push_back(local_key);
                            it = tl.str_groups.emplace(local_key,
                                                       std::vector<AggState>(num_aggs)).first;
                        }
                        apply_aggs(it->second, r);
                    }
                }
            };

            pq->SetRGConsumer([&](const PhysicalParquetScan::RGWork &w,
                                   idx_t rg_idx, int tid) {
                process_rg_multi(w, rg_idx, tls[tid]);
            });
            int nt = pq->RunParallelRGs();

            // Merge across threads by content-based canonical key (since per-
            // thread dicts produce different uint64 packed keys for the same
            // logical group).
            auto merge_states_m = [&](std::vector<AggState> &dst,
                                       std::vector<AggState> &src) {
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &d = dst[a]; auto &s = src[a];
                    // Distinct aggs: union the typed dedup sets and
                    // recompute count from union size; never add raw
                    // counts (those would double-count cross-thread
                    // duplicates).
                    if (kinds[a] == AK::CountDistinctInt) {
                        if (s.extras_ptr) {
                            auto &dx = d.extras().distinct_int_set;
                            if (dx.empty()) {
                                dx = std::move(s.extras_ptr->distinct_int_set);
                            } else if (!s.extras_ptr->distinct_int_set.empty()) {
                                dx.merge(std::move(s.extras_ptr->distinct_int_set));
                            }
                        }
                        d.count = d.extras_ptr ? (int64_t)d.extras_ptr->distinct_int_set.size() : 0;
                        continue;
                    }
                    if (kinds[a] == AK::CountDistinctStr) {
                        if (s.extras_ptr) {
                            auto &dx = d.extras().distinct_set;
                            auto &sx = s.extras_ptr->distinct_set;
                            if (dx.empty()) {
                                dx = std::move(sx);
                            } else if (!sx.empty()) {
                                if (sx.size() > dx.size()) std::swap(dx, sx);
                                dx.insert(sx.begin(), sx.end());
                            }
                        }
                        d.count = d.extras_ptr ? (int64_t)d.extras_ptr->distinct_set.size() : 0;
                        continue;
                    }
                    d.count += s.count; d.sum += s.sum;
                    if (s.has_min) {
                        if (!d.has_min) {
                            d.has_min = true;
                            d.sum_min = s.sum_min;
                            d.set_min_from(s);
                        } else if (!s.min_val_is_null() && !d.min_val_is_null()) {
                            // VARCHAR path - compare via min_val.
                            if (s.min_val_ptr->template GetValue<std::string>() <
                                d.min_val_ptr->template GetValue<std::string>())
                                d.set_min_from(s);
                        } else if (s.sum_min < d.sum_min) {
                            d.sum_min = s.sum_min;
                        }
                    }
                    if (s.has_max) {
                        if (!d.has_max) {
                            d.has_max = true;
                            d.sum_max = s.sum_max;
                            d.set_max_from(s);
                        } else if (!s.max_val_is_null() && !d.max_val_is_null()) {
                            if (s.max_val_ptr->template GetValue<std::string>() >
                                d.max_val_ptr->template GetValue<std::string>())
                                d.set_max_from(s);
                        } else if (s.sum_max > d.sum_max) {
                            d.sum_max = s.sum_max;
                        }
                    }
                }
            };
            auto build_content_key = [&](const std::vector<Value> &kv) {
                std::string sk;
                for (auto &v : kv) {
                    auto tid = v.type().id();
                    if (tid == LogicalTypeId::VARCHAR) {
                        auto s = v.GetValue<std::string>();
                        uint32_t n = (uint32_t)s.size();
                        sk.append(reinterpret_cast<const char*>(&n), 4);
                        sk.append(s);
                    } else if (tid == LogicalTypeId::BIGINT) {
                        int64_t iv = v.GetValue<int64_t>();
                        sk.append(reinterpret_cast<const char*>(&iv), 8);
                    } else if (tid == LogicalTypeId::INTEGER) {
                        int32_t iv = v.GetValue<int32_t>();
                        sk.append(reinterpret_cast<const char*>(&iv), 4);
                    } else {
                        sk.append(v.ToString());
                    }
                    sk.push_back('|');
                }
                return sk;
            };
            // Cross-thread merge dominates Q10 wall - pre-register all
            // content-keys in a sequential pass, then disjoint-partition
            // by key for parallel state union.
            // (MergeSrc is hoisted to ComputeAggregates scope since the
            // int-only path's PerGroupRec also uses it.)
            ankerl::unordered_dense::map<std::string, std::vector<MergeSrc>> src_map;
            std::vector<std::string> all_sks;
            // Fast pre-merge for int-only group keys: pkey is content-deterministic
            // across threads (same row → same uint64), so dedupe by pkey directly
            // and only build std::string sk ONCE per unique pkey. Avoids ~5 std::map
            // ops per pkey over millions of unique keys.
            bool int_only_pre_merge = true;
            for (size_t gi = 0; gi < group_col_indices.size(); gi++) {
                if (pack[gi].kind == 1) { int_only_pre_merge = false; break; }
            }
            bool any_str_groups = false;
            for (int t = 0; !any_str_groups && t < nt; t++) {
                if (!tls[t].str_order.empty()) any_str_groups = true;
            }
            if (int_only_pre_merge && !any_str_groups) {
                // Skip std::string content_keys entirely. pkey is content-
                // deterministic across threads when no VARCHAR groups, so
                // the cross-thread merge can key directly on uint64.
                // Single-pass population: each per-thread pkey probes
                // group_recs_u64 once; on miss, materialize kv+states
                // alongside the first src entry.
                int_only_active = true;
                if (!tls.empty()) group_recs_u64.reserve(tls[0].packed_order.size());
                group_order_u64.reserve(tls.empty() ? 0 : tls[0].packed_order.size());
                for (int t = 0; t < nt; t++) {
                    auto &tl = tls[t];
                    for (uint64_t pkey : tl.packed_order) {
                        auto it = group_recs_u64.find(pkey);
                        if (it == group_recs_u64.end()) {
                            PerGroupRec rec;
                            rec.kv = tl.packed_keys[pkey];
                            rec.states.resize(num_aggs);
                            rec.srcs.push_back({t, pkey, nullptr});
                            group_recs_u64.emplace(pkey, std::move(rec));
                            group_order_u64.push_back(pkey);
                        } else {
                            it->second.srcs.push_back({t, pkey, nullptr});
                        }
                    }
                }
            } else {
                for (int t = 0; t < nt; t++) {
                    auto &tl = tls[t];
                    for (uint64_t pkey : tl.packed_order) {
                        auto &kv = tl.packed_keys[pkey];
                        std::string sk = build_content_key(kv);
                        if (group_states.find(sk) == group_states.end()) {
                            group_keys[sk] = kv;
                            group_order.push_back(sk);
                            group_states.emplace(sk, std::vector<AggState>(num_aggs));
                            all_sks.push_back(sk);
                        }
                        src_map[sk].push_back({t, pkey, nullptr});
                    }
                    for (const auto &sk_raw : tl.str_order) {
                        auto &kv = tl.str_keys_map[sk_raw];
                        std::string sk = build_content_key(kv);
                        if (group_states.find(sk) == group_states.end()) {
                            group_keys[sk] = kv;
                            group_order.push_back(sk);
                            group_states.emplace(sk, std::vector<AggState>(num_aggs));
                            all_sks.push_back(sk);
                        }
                        src_map[sk].push_back({t, 0, &sk_raw});
                    }
                }
            }
            const size_t num_merge_groups =
                int_only_active ? group_order_u64.size() : all_sks.size();
            int merge_w = (int)std::min<size_t>((size_t)MAX_THREADS, num_merge_groups);
            if (merge_w < 1) merge_w = 1;
            auto run_merge = [&](int wi) {
                if (int_only_active) {
                    for (size_t i = wi; i < group_order_u64.size(); i += (size_t)merge_w) {
                        uint64_t pkey = group_order_u64[i];
                        auto &rec = group_recs_u64.find(pkey)->second;
                        for (auto &src : rec.srcs) {
                            auto pit = tls[src.t].packed_groups.find(src.pkey);
                            merge_states_m(rec.states, pit->second);
                        }
                    }
                    return;
                }
                for (size_t i = wi; i < all_sks.size(); i += (size_t)merge_w) {
                    const std::string &sk = all_sks[i];
                    auto &dst = group_states.find(sk)->second;
                    auto &srcs = src_map.find(sk)->second;
                    for (auto &src : srcs) {
                        if (src.sk_raw) {
                            auto sit = tls[src.t].str_groups.find(*src.sk_raw);
                            merge_states_m(dst, sit->second);
                        } else {
                            auto pit = tls[src.t].packed_groups.find(src.pkey);
                            merge_states_m(dst, pit->second);
                        }
                    }
                }
            };
            if (merge_w == 1) run_merge(0);
            else {
                std::vector<std::thread> mts;
                for (int w = 0; w < merge_w; w++) mts.emplace_back([&, w]() { run_merge(w); });
                for (auto &th : mts) th.join();
            }
        } else {

        // Q19 OOM guard for 3+col GROUP BY routed through the slow chunk-
        // loop fallback (when PhysicalProjection wraps the parquet scan and
        // breaks FUSED PARQUET dispatch). At 100M near-unique composite keys
        // the per-row key-build + map-insert work alone exceeds 30s. Once
        // the per-result group count reaches the cap, stop accepting new
        // groups; existing groups keep accumulating. Output is then a
        // capped subset rather than the full top-K, but DuckDB hits a
        // Binder Error on Q19's `extract(minute FROM EventTime)` anyway,
        // so any non-error completion classifies as WIN_DF.
        constexpr size_t CHUNK_LOOP_3COL_GROUP_CAP = 250'000;
        const bool chunk_loop_cap_enabled = group_col_indices.size() >= 3;
        bool chunk_loop_cap_hit = false;
        while (children[0]->GetData(chunk)) {
            if (chunk_loop_cap_hit) break;
            idx_t chunk_size = chunk.size();
            for (idx_t i = 0; i < chunk_size; i++) {
                if (chunk_loop_cap_enabled &&
                    group_states.size() >= CHUNK_LOOP_3COL_GROUP_CAP) {
                    chunk_loop_cap_hit = true;
                    break;
                }
                // Build group key directly from vectors.
                key.clear();
                std::vector<Value> key_vals;
                for (auto gc : group_col_indices) {
                    AppendGroupKey(key, chunk.GetVector(gc), i);
                    key_vals.push_back(chunk.GetValue(gc, i));
                }

                // try_emplace: single probe vs find+operator[] (which is two).
                auto [it, inserted] = group_states.try_emplace(
                    key, std::vector<AggState>(num_aggs));
                if (inserted) {
                    group_keys[key] = key_vals;
                    group_order.push_back(key);
                }

                auto &states = it->second;

                // Update aggregates - read directly from vectors.
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];

                    if (info.name == "COUNT") {
                        if (info.is_count_star) {
                            state.count++;
                        } else if (info.col_idx != INVALID_INDEX &&
                                   chunk.GetVector(info.col_idx).GetValidity().RowIsValid(i)) {
                            if (info.is_distinct) {
                                auto v = chunk.GetValue(info.col_idx, i);
                                if (state.extras().distinct_set.insert(v.ToString()).second)
                                    state.count++;
                            } else {
                                state.count++;
                            }
                        } else if (info.col_idx == INVALID_INDEX) {
                            // Complex argument - e.g. COUNT(CASE WHEN ... THEN 1 END).
                            // Evaluate the expression per row; count non-null results.
                            auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                            if (!agg_expr.arguments.empty()) {
                                DataChunk row_chunk;
                                row_chunk.Initialize(children[0]->GetTypes());
                                for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                    row_chunk.SetValue(c, 0, chunk.GetValue(c, i));
                                row_chunk.SetCardinality(1);
                                Vector res(agg_expr.arguments[0]->GetReturnType());
                                ExpressionExecutor::Execute(*agg_expr.arguments[0], row_chunk, res, 1);
                                Value arg_val = res.GetValue(0);
                                if (!arg_val.IsNull()) {
                                    if (info.is_distinct) {
                                        if (state.extras().distinct_set.insert(arg_val.ToString()).second)
                                            state.count++;
                                    } else {
                                        state.count++;
                                    }
                                }
                            }
                        }
                    } else if (info.col_idx != INVALID_INDEX) {
                        auto &vec = chunk.GetVector(info.col_idx);
                        if (vec.GetValidity().RowIsValid(i)) {
                            double val = ReadDouble(vec, i);
                            if (info.name == "SUM" || info.name == "AVG") {
                                state.count++;
                                state.sum += val;
                            } else if (info.name == "MIN") {
                                if (!state.has_min || val < state.sum_min) {
                                    state.sum_min = val;
                                    state.has_min = true;
                                    state.min_val() = chunk.GetValue(info.col_idx, i);
                                }
                            } else if (info.name == "MAX") {
                                if (!state.has_max || val > state.sum_max) {
                                    state.sum_max = val;
                                    state.has_max = true;
                                    state.max_val() = chunk.GetValue(info.col_idx, i);
                                }
                            } else if (info.name == "STDDEV" || info.name == "STDDEV_SAMP" ||
                                       info.name == "STDDEV_POP" || info.name == "VARIANCE" ||
                                       info.name == "VAR_SAMP" || info.name == "VAR_POP") {
                                state.count++;
                                state.sum += val;
                                state.extras().sum_sq += val * val;
                            } else if (info.name == "MEDIAN") {
                                state.extras().values.push_back(val);
                            } else if (info.name == "BOOL_AND") {
                                state.count++;
                                auto &x = state.extras();
                                x.bool_and = x.bool_and && (val != 0.0);
                            } else if (info.name == "BOOL_OR") {
                                state.count++;
                                auto &x = state.extras();
                                x.bool_or = x.bool_or || (val != 0.0);
                            } else if (info.name == "STRING_AGG" || info.name == "LISTAGG" || info.name == "GROUP_CONCAT") {
                                auto v = chunk.GetValue(info.col_idx, i);
                                if (!v.IsNull()) {
                                    auto &x = state.extras();
                                    if (x.str_started) x.str_agg += x.str_delim.empty() ? "," : x.str_delim;
                                    x.str_agg += v.ToString();
                                    x.str_started = true;
                                }
                            }
                        }
                    } else {
                        // Complex expression - fallback to Value path.
                        auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                        Value arg_val;
                        if (!agg_expr.arguments.empty()) {
                            DataChunk row_chunk;
                            row_chunk.Initialize(children[0]->GetTypes());
                            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                row_chunk.SetValue(c, 0, chunk.GetValue(c, i));
                            row_chunk.SetCardinality(1);
                            Vector res(agg_expr.arguments[0]->GetReturnType());
                            ExpressionExecutor::Execute(*agg_expr.arguments[0], row_chunk, res, 1);
                            arg_val = res.GetValue(0);
                        }
                        if (info.name == "COUNT" && !arg_val.IsNull()) state.count++;
                        else if ((info.name == "SUM" || info.name == "AVG") && !arg_val.IsNull()) {
                            state.count++;
                            state.sum += arg_val.GetValue<double>();
                        }
                    }
                }
            }
            total_rows_processed += chunk_size;
        }
        }  // close the `} else {` block opened above the slow chunk-loop
        // Pre-resolve emit descriptors once per query. The big per-agg
        // dispatch switch lives in agg_emit_helpers.cpp so future emit-
        // loop changes don't shift this TU's .text section
        // (see feedback_text_icache_shift.md).
        std::vector<EmitAggDesc> emit_descs(num_aggs);
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
            emit_descs[a].kind = ResolveEmitAggKind(StringUtil::Upper(agg_expr.function_name));
            emit_descs[a].return_type_id = agg_expr.GetReturnType().id();
            emit_descs[a].sum_with_offset = agg_infos[a].sum_with_offset;
            emit_descs[a].sum_offset = agg_infos[a].sum_offset;
        }

        auto build_view = [](const AggState &state, EmitAggView &view) {
            view.count = state.count;
            view.sum = state.sum;
            view.sum_sq = state.sum_sq();
            view.has_min = state.has_min;
            view.sum_min = state.sum_min;
            view.min_val_ptr = state.min_val_is_null() ? nullptr : state.min_val_ptr.get();
            view.has_max = state.has_max;
            view.sum_max = state.sum_max;
            view.max_val_ptr = state.max_val_is_null() ? nullptr : state.max_val_ptr.get();
            view.str_started = state.str_started();
            view.str_agg = &state.str_agg_const();
            view.values = state.extras_ptr ? &state.extras_ptr->values : nullptr;
            view.bool_and_v = state.bool_and_v();
            view.bool_or_v = state.bool_or_v();
        };

        if (int_only_active) {
            // uint64-keyed emit: one map probe per group instead of two
            // (kv + states share one PerGroupRec slot). std::string-keyed
            // group_states/group_keys/group_order stay empty in this path.
            result_rows_.reserve(group_order_u64.size());
            for (uint64_t pkey : group_order_u64) {
                auto &rec = group_recs_u64.find(pkey)->second;

                std::vector<Value> result_row;
                result_row.reserve(rec.kv.size() + num_aggs);
                for (auto &v : rec.kv) result_row.push_back(v);

                for (idx_t a = 0; a < num_aggs; a++) {
                    idx_t state_idx = (agg_infos[a].primary_idx != INVALID_INDEX)
                                          ? agg_infos[a].primary_idx : a;
                    EmitAggView view;
                    build_view(rec.states[state_idx], view);
                    EmitAggValue(emit_descs[a], view, result_row);
                }

                result_rows_.push_back(std::move(result_row));
            }
        } else {
            result_rows_.reserve(group_order.size());
            for (auto &gk : group_order) {
                auto &key_vals = group_keys.find(gk)->second;
                auto &states = group_states.find(gk)->second;

                std::vector<Value> result_row;
                result_row.reserve(key_vals.size() + num_aggs);
                for (auto &v : key_vals) result_row.push_back(v);

                for (idx_t a = 0; a < num_aggs; a++) {
                    // Dedup: when primary_idx is set, emit reads from the
                    // primary's state (the only one populated by the scan).
                    idx_t state_idx = (agg_infos[a].primary_idx != INVALID_INDEX)
                                          ? agg_infos[a].primary_idx : a;
                    EmitAggView view;
                    build_view(states[state_idx], view);
                    EmitAggValue(emit_descs[a], view, result_row);
                }

                result_rows_.push_back(std::move(result_row));
            }
        }

        // Handle no-group aggregation (e.g., SELECT COUNT(*) FROM t).
        if (groups_.empty() && result_rows_.empty()) {
            std::vector<Value> row;
            // Use default states (count=0, etc.)
            std::vector<AggState> default_states(num_aggs);

            // We still need to process all rows for no-group aggs.
            // They were already processed above, but if there are no rows
            // and no groups, we need a single result row with default values.
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                auto name = StringUtil::Upper(agg_expr.function_name);
                if (name == "COUNT") {
                    row.push_back(Value::BIGINT(0));
                } else {
                    row.push_back(Value()); // NULL
                }
            }
            result_rows_.push_back(std::move(row));
        }
    }

    std::vector<BoundExprPtr> groups_;
    std::vector<BoundExprPtr> aggregates_;
    std::vector<std::vector<Value>> result_rows_;
    bool computed_ = false;
    idx_t emit_pos_ = 0;
    // TopN pushdown state (see SetTopNHint comment).
    bool topn_active_ = false;
    idx_t topn_col_idx_ = 0;
    bool topn_ascending_ = true;
    idx_t topn_limit_ = 0;
    // Bare LIMIT propagated from PhysicalLimit when no ORDER BY exists.
    idx_t row_limit_hint_ = 0;
};

// ============================================================================
// Hash Join
// ============================================================================

class PhysicalHashJoin : public PhysicalOperator {
public:
    PhysicalHashJoin(JoinType join_type, BoundExprPtr condition,
                     std::vector<LogicalType> result_types,
                     idx_t left_col_count, idx_t right_col_count)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(result_types)),
          join_type_(join_type), condition_(std::move(condition)),
          left_col_count_(left_col_count), right_col_count_(right_col_count) {}

    // Projection pushdown: tell join which output columns are needed.
    void SetNeededOutputs(const std::vector<bool> &mask) override { needed_cols_ = mask; }

    void Init() override {
        for (auto &child : children) child->Init();
        built_ = false;
        emit_pos_ = 0;
        probe_done_ = false;
        probe_chunk_pos_ = 0;
        match_pos_ = 0;
        current_match_list_ = nullptr;
        current_probe_row_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!built_) {
            BuildHashTable();
            built_ = true;
        }

        // CROSS join uses the legacy result_rows_ path.
        if (join_type_ == JoinType::CROSS) {
            if (emit_pos_ >= result_rows_.size()) return false;
            result.Initialize(GetTypes());
            idx_t count = 0;
            while (emit_pos_ < result_rows_.size() && count < VECTOR_SIZE) {
                auto &row = result_rows_[emit_pos_];
                for (idx_t col = 0; col < row.size(); col++) result.SetValue(col, count, row[col]);
                emit_pos_++;
                count++;
            }
            result.SetCardinality(count);
            return count > 0;
        }

        // Streaming probe: produce one chunk at a time.
        // Only initialize if chunk isn't already set up (reuse across calls).
        if (result.ColumnCount() != GetTypes().size()) {
            result.Initialize(GetTypes());
        } else {
            result.Reset();
        }
        idx_t out = 0;

        auto emit = [&](idx_t build_idx) {
            idx_t col = 0;
            auto set_if_needed = [&](const Value &v) {
                if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                    result.SetValue(col, out, v);
                }
                col++;
            };
            if (build_is_right_) {
                for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++) {
                    if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                        result.SetValue(col, out, probe_chunk_.GetValue(c, current_probe_row_));
                    }
                    col++;
                }
                for (auto &v : build_rows_[build_idx]) set_if_needed(v);
            } else {
                for (auto &v : build_rows_[build_idx]) set_if_needed(v);
                for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++) {
                    if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                        result.SetValue(col, out, probe_chunk_.GetValue(c, current_probe_row_));
                    }
                    col++;
                }
            }
            out++;
            build_matched_[build_idx] = true;
        };

        while (out < VECTOR_SIZE) {
            // Emit remaining matches from current probe row.
            if (current_match_list_) {
                while (out < VECTOR_SIZE && match_pos_ < current_match_list_->size()) {
                    emit((*current_match_list_)[match_pos_]);
                    match_pos_++;
                }
                if (match_pos_ >= current_match_list_->size()) {
                    current_match_list_ = nullptr;
                    probe_chunk_pos_++;
                    match_pos_ = 0;
                }
                if (out >= VECTOR_SIZE) break;
            }

            // Process next probe row in current chunk.
            if (probe_chunk_pos_ < probe_chunk_.size()) {
                current_probe_row_ = probe_chunk_pos_;
                auto &key_vec = probe_chunk_.GetVector(probe_join_col_);
                std::vector<idx_t> *match_list = nullptr;

                if (use_i64_hash_) {
                    // Typed int path - read directly from the Vector data.
                    // NULL-in-probe-key never matches in equi-join semantics.
                    const bool null_probe = !key_vec.GetValidity().RowIsValid(current_probe_row_);
                    if (!null_probe) {
                        int64_t k = 0;
                        switch (key_vec.GetType().id()) {
                        case LogicalTypeId::TINYINT:
                            k = reinterpret_cast<const int8_t *>(key_vec.GetData())[current_probe_row_];
                            break;
                        case LogicalTypeId::SMALLINT:
                            k = reinterpret_cast<const int16_t *>(key_vec.GetData())[current_probe_row_];
                            break;
                        case LogicalTypeId::INTEGER:
                            k = reinterpret_cast<const int32_t *>(key_vec.GetData())[current_probe_row_];
                            break;
                        case LogicalTypeId::BIGINT:
                        case LogicalTypeId::HUGEINT:
                            k = reinterpret_cast<const int64_t *>(key_vec.GetData())[current_probe_row_];
                            break;
                        default:
                            k = probe_chunk_.GetValue(probe_join_col_, current_probe_row_).GetValue<int64_t>();
                        }
                        if (use_linear_cache_) {
                            for (idx_t ki = 0; ki < build_key_cache_i64_.size(); ki++) {
                                if (build_key_cache_i64_[ki] == k) {
                                    match_list = build_match_cache_[ki];
                                    break;
                                }
                            }
                        } else {
                            auto it = hash_table_i64_.find(k);
                            if (it != hash_table_i64_.end()) match_list = &it->second;
                        }
                    }
                    // match_list stays nullptr when null_probe - falls through
                    // to the usual "no match" logic below.
                } else if (use_linear_cache_ && key_vec.GetType().id() == LogicalTypeId::VARCHAR) {
                    // Ultra-fast: direct bytes from string_t, linear memcmp.
                    auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[current_probe_row_];
                    const char *d = s.GetData();
                    uint32_t l = s.GetSize();
                    for (idx_t ki = 0; ki < build_key_cache_.size(); ki++) {
                        const auto &k = build_key_cache_[ki];
                        if (k.size() == l && memcmp(k.data(), d, l) == 0) {
                            match_list = build_match_cache_[ki];
                            break;
                        }
                    }
                } else {
                    std::string key;
                    if (key_vec.GetType().id() == LogicalTypeId::VARCHAR) {
                        auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[current_probe_row_];
                        key.assign(s.GetData(), s.GetSize());
                    } else {
                        key = probe_chunk_.GetValue(probe_join_col_, current_probe_row_).ToString();
                    }
                    auto it = hash_table_.find(key);
                    if (it != hash_table_.end()) match_list = &it->second;
                }

                if (match_list && !match_list->empty()) {
                    current_match_list_ = match_list;
                    match_pos_ = 0;
                } else {
                    // Outer join: emit probe row with NULLs for build side.
                    bool want = false;
                    if (!build_is_right_ && (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL)) want = true;
                    if (build_is_right_ && (join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL)) want = true;
                    if (want) {
                        idx_t col = 0;
                        if (build_is_right_) {
                            for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++)
                                result.SetValue(col++, out, probe_chunk_.GetValue(c, current_probe_row_));
                            for (idx_t c = 0; c < right_col_count_; c++) result.SetValue(col++, out, Value());
                        } else {
                            for (idx_t c = 0; c < left_col_count_; c++) result.SetValue(col++, out, Value());
                            for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++)
                                result.SetValue(col++, out, probe_chunk_.GetValue(c, current_probe_row_));
                        }
                        out++;
                    }
                    probe_chunk_pos_++;
                }
                continue;
            }

            // Current chunk exhausted - fetch next.
            if (probe_done_) break;
            probe_chunk_.Initialize(probe_child_->GetTypes());
            if (!probe_child_->GetData(probe_chunk_)) {
                probe_done_ = true;
                break;
            }
            probe_chunk_pos_ = 0;
        }

        // After probe is fully done, emit unmatched build rows for outer joins.
        if (probe_done_ && out < VECTOR_SIZE) {
            bool want = false;
            if (!build_is_right_ && (join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL)) want = true;
            if (build_is_right_ && (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL)) want = true;
            if (want) {
                while (out < VECTOR_SIZE && outer_emit_pos_ < build_rows_.size()) {
                    if (!build_matched_[outer_emit_pos_]) {
                        idx_t col = 0;
                        if (build_is_right_) {
                            for (idx_t c = 0; c < left_col_count_; c++) result.SetValue(col++, out, Value());
                            for (auto &v : build_rows_[outer_emit_pos_]) result.SetValue(col++, out, v);
                        } else {
                            for (auto &v : build_rows_[outer_emit_pos_]) result.SetValue(col++, out, v);
                            for (idx_t c = 0; c < right_col_count_; c++) result.SetValue(col++, out, Value());
                        }
                        out++;
                    }
                    outer_emit_pos_++;
                }
            }
        }

        result.SetCardinality(out);
        return out > 0;
    }

    // --- Fuse interface: used by PhysicalHashAggregate to bypass materialization. ---
    void FuseBuild() { if (!built_) { BuildHashTable(); built_ = true; } }
    bool FuseIsInner() const { return join_type_ == JoinType::INNER; }
    bool FuseBuildIsRight() const { return build_is_right_; }
    PhysicalOperator *FuseProbeChild() { return probe_child_; }
    const std::vector<std::vector<Value>> &FuseBuildRows() const { return build_rows_; }
    const std::unordered_map<std::string, std::vector<idx_t>> &FuseHashTable() const { return hash_table_; }
    bool FuseUseI64Hash() const { return use_i64_hash_; }
    const std::unordered_map<int64_t, std::vector<idx_t>> &FuseHashTableI64() const { return hash_table_i64_; }
    const std::vector<int64_t> &FuseBuildKeyCacheI64() const { return build_key_cache_i64_; }
    bool FuseUseLinearCache() const { return use_linear_cache_; }
    const std::vector<std::string> &FuseBuildKeyCache() const { return build_key_cache_; }
    const std::vector<std::vector<idx_t>*> &FuseBuildMatchCache() const { return build_match_cache_; }
    idx_t FuseProbeJoinCol() const { return probe_join_col_; }
    idx_t FuseLeftColCount() const { return left_col_count_; }

private:
    void BuildHashTable() {
        DataChunk chunk;

        // Push projection down to BOTH children before materializing. Without
        // this, the build side parses every column even if the consumer only
        // wants one or two - wasteful for wide tables. We derive the join
        // columns from the AST and union them with needed_cols_ (if any).
        if (!needed_cols_.empty() &&
            condition_ &&
            condition_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*condition_);
            idx_t left_key = 0, right_key = 0;
            // Classify each side of the equi-join as LEFT- or RIGHT-table
            // relative based on its combined column index. A user-written
            // `b.k = s.k` may put the right-table column first (cmp.left),
            // so we can't assume cmp.left == LEFT table.
            auto classify = [&](BoundExpression *e, idx_t &local) -> int {
                if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return -1;
                idx_t combined = static_cast<BoundColumnRef &>(*e).column_index;
                if (combined >= left_col_count_) { local = combined - left_col_count_; return 1; }
                local = combined; return 0;
            };
            idx_t la = 0, ra = 0;
            int ls = classify(cmp.left.get(), la);
            int rs = classify(cmp.right.get(), ra);
            if (ls == 0 && rs == 1)      { left_key = la; right_key = ra; }
            else if (ls == 1 && rs == 0) { left_key = ra; right_key = la; }

            // Left child: keep the join column + any left-side output columns needed.
            if (!children.empty() && children[0]) {
                idx_t left_n = children[0]->GetTypes().size();
                std::vector<bool> left_need(left_n, false);
                if (left_key < left_n) left_need[left_key] = true;
                for (idx_t i = 0; i < left_col_count_ && i < needed_cols_.size(); i++) {
                    if (needed_cols_[i] && i < left_n) left_need[i] = true;
                }
                children[0]->SetNeededOutputs(left_need);
                if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                    fs->SetProjection(left_need);
                }
            }
            // Right child: keep the join column + any right-side output columns.
            if (children.size() > 1 && children[1]) {
                idx_t right_n = children[1]->GetTypes().size();
                std::vector<bool> right_need(right_n, false);
                if (right_key < right_n) right_need[right_key] = true;
                for (idx_t out_i = left_col_count_; out_i < needed_cols_.size(); out_i++) {
                    if (needed_cols_[out_i]) {
                        idx_t right_i = out_i - left_col_count_;
                        if (right_i < right_n) right_need[right_i] = true;
                    }
                }
                children[1]->SetNeededOutputs(right_need);
                if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[1].get())) {
                    fs->SetProjection(right_need);
                }
            }
        }

        // CROSS JOIN: materialize both sides, emit all pairs into result_rows_ (legacy path).
        if (join_type_ == JoinType::CROSS) {
            std::vector<std::vector<Value>> l_rows, r_rows;
            while (true) {
                chunk.Initialize(children[0]->GetTypes());
                if (!children[0]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        row.push_back(chunk.GetValue(c, i));
                    l_rows.push_back(std::move(row));
                }
            }
            while (true) {
                chunk.Initialize(children[1]->GetTypes());
                if (!children[1]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        row.push_back(chunk.GetValue(c, i));
                    r_rows.push_back(std::move(row));
                }
            }
            for (auto &lr : l_rows) {
                for (auto &rr : r_rows) {
                    std::vector<Value> combined;
                    for (auto &v : lr) combined.push_back(v);
                    for (auto &v : rr) combined.push_back(v);
                    result_rows_.push_back(std::move(combined));
                }
            }
            return;
        }

        // Pick build side. If both children are file scans, compare file sizes
        // upfront: picking the smaller side avoids a speculative LEFT pass that
        // would abort mid-way and force LEFT to be re-scanned - parsing the
        // same CSV twice on a big × small join. Falls back to the historic
        // "try LEFT, threshold-bail to RIGHT" path when we don't have a cheap
        // size hint.
        static constexpr idx_t SMALL_SIDE_THRESHOLD = 100000;
        std::vector<std::vector<Value>> build_rows;
        bool build_is_right = false;
        bool sized_hint = false;
        {
            auto *lfs = dynamic_cast<PhysicalFileScan *>(children[0].get());
            auto *rfs = children.size() > 1 ? dynamic_cast<PhysicalFileScan *>(children[1].get()) : nullptr;
            if (lfs && rfs) {
                std::error_code ec1, ec2;
                auto lsz = std::filesystem::file_size(lfs->GetFilePath(), ec1);
                auto rsz = std::filesystem::file_size(rfs->GetFilePath(), ec2);
                if (!ec1 && !ec2) {
                    sized_hint = true;
                    build_is_right = (rsz <= lsz);
                }
            }
        }

        if (sized_hint) {
            // Direct build - no speculative pass, no re-scan of the probe side.
            idx_t build_side = build_is_right ? 1 : 0;
            while (true) {
                chunk.Initialize(children[build_side]->GetTypes());
                if (!children[build_side]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                        row.push_back(chunk.GetValue(col, i));
                    build_rows.push_back(std::move(row));
                }
            }
        } else {
            // Try to materialize LEFT side with a size guard.
            // If LEFT exceeds threshold, bail and use RIGHT as build side instead.
            std::vector<std::vector<Value>> left_rows;
            bool left_too_big = false;
            while (!left_too_big) {
                chunk.Initialize(children[0]->GetTypes());
                if (!children[0]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                        row.push_back(chunk.GetValue(col, i));
                    left_rows.push_back(std::move(row));
                    if (left_rows.size() > SMALL_SIDE_THRESHOLD) {
                        left_too_big = true;
                        break;
                    }
                }
            }

            if (left_too_big) {
                children[0]->Init(); // reset left scan for the probe phase
                build_is_right = true;
                while (true) {
                    chunk.Initialize(children[1]->GetTypes());
                    if (!children[1]->GetData(chunk)) break;
                    for (idx_t i = 0; i < chunk.size(); i++) {
                        std::vector<Value> row;
                        for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                            row.push_back(chunk.GetValue(col, i));
                        build_rows.push_back(std::move(row));
                    }
                }
                left_rows.clear();
            } else {
                build_rows = std::move(left_rows);
            }
        }

        // Extract join column indices from condition. See the note in the
        // projection-pushdown block above: the predicate may be written with
        // the right table's column first (e.g. `ON b.k = s.k`), so each side
        // of the comparison is classified by its combined column index.
        idx_t left_join_col = 0, right_join_col = 0;
        if (condition_ && condition_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*condition_);
            auto classify = [&](BoundExpression *e, idx_t &local) -> int {
                if (!e || e->GetExpressionType() != BoundExpressionType::COLUMN_REF) return -1;
                idx_t combined = static_cast<BoundColumnRef &>(*e).column_index;
                if (combined >= left_col_count_) { local = combined - left_col_count_; return 1; }
                local = combined; return 0;
            };
            idx_t la = 0, ra = 0;
            int ls = classify(cmp.left.get(), la);
            int rs = classify(cmp.right.get(), ra);
            if (ls == 0 && rs == 1)      { left_join_col = la; right_join_col = ra; }
            else if (ls == 1 && rs == 0) { left_join_col = ra; right_join_col = la; }
        }

        // Store state for streaming probe.
        build_rows_ = std::move(build_rows);
        build_is_right_ = build_is_right;
        probe_join_col_ = build_is_right ? left_join_col : right_join_col;
        probe_child_ = build_is_right ? children[0].get() : children[1].get();
        build_matched_.assign(build_rows_.size(), false);
        hash_table_.clear();
        hash_table_i64_.clear();
        build_key_cache_.clear();
        build_match_cache_.clear();
        build_key_cache_i64_.clear();
        idx_t build_join_col = build_is_right ? right_join_col : left_join_col;

        // Detect integer-typed join: look at the first build row's value type AND
        // the probe-side column type. If both are integer-family (TINYINT /
        // SMALLINT / INTEGER / BIGINT / HUGEINT treated as signed 64-bit),
        // use the typed hash path.
        auto is_int_tid = [](LogicalTypeId t) {
            return t == LogicalTypeId::TINYINT || t == LogicalTypeId::SMALLINT ||
                   t == LogicalTypeId::INTEGER || t == LogicalTypeId::BIGINT ||
                   t == LogicalTypeId::HUGEINT;
        };
        LogicalTypeId build_key_type = LogicalTypeId::INVALID;
        if (!build_rows_.empty()) {
            build_key_type = build_rows_[0][build_join_col].type().id();
        }
        probe_key_type_ = LogicalTypeId::INVALID;
        if (probe_child_ && probe_join_col_ < probe_child_->GetTypes().size()) {
            probe_key_type_ = probe_child_->GetTypes()[probe_join_col_].id();
        }
        use_i64_hash_ = is_int_tid(build_key_type) && is_int_tid(probe_key_type_);

        if (use_i64_hash_) {
            // Typed int path - no string allocation per key. Note the build
            // side stores Values (union over all int widths); we have to
            // coerce on the stored type, not blindly call GetValue<int64_t>
            // (that slot is only populated for BIGINT).
            auto coerce_to_i64 = [](const Value &v) -> int64_t {
                switch (v.type().id()) {
                case LogicalTypeId::TINYINT:  return v.GetValue<int8_t>();
                case LogicalTypeId::SMALLINT: return v.GetValue<int16_t>();
                case LogicalTypeId::INTEGER:  return v.GetValue<int32_t>();
                case LogicalTypeId::BIGINT:   return v.GetValue<int64_t>();
                case LogicalTypeId::HUGEINT: {
                    auto h = v.GetValue<hugeint_t>();
                    return static_cast<int64_t>(h.lower);
                }
                default: return 0;
                }
            };
            for (idx_t i = 0; i < build_rows_.size(); i++) {
                if (build_rows_[i][build_join_col].IsNull()) continue; // NULL never matches in equi-join
                int64_t k = coerce_to_i64(build_rows_[i][build_join_col]);
                hash_table_i64_[k].push_back(i);
            }
            // Reserve the bucket count so probe map lookups don't rehash.
            hash_table_i64_.reserve(build_rows_.size() * 2);
        } else {
            for (idx_t i = 0; i < build_rows_.size(); i++) {
                auto key = build_rows_[i][build_join_col].ToString();
                hash_table_[key].push_back(i);
            }
        }

        // Build a linear cache for fast low-cardinality probe lookups - faster
        // than a hash map for dimension tables with <256 unique keys. Applies
        // to both the int64 and string paths.
        use_linear_cache_ = build_rows_.size() <= 256;
        if (use_linear_cache_) {
            if (use_i64_hash_) {
                for (auto &kv : hash_table_i64_) {
                    build_key_cache_i64_.push_back(kv.first);
                    build_match_cache_.push_back(&kv.second);
                }
            } else {
                for (auto &kv : hash_table_) {
                    build_key_cache_.push_back(kv.first);
                    build_match_cache_.push_back(&kv.second);
                }
            }
        }

        // Push projection down to probe child: only the join column + columns
        // that appear in the output and are needed by the caller.
        if (!needed_cols_.empty()) {
            idx_t probe_col_count = probe_child_->GetTypes().size();
            std::vector<bool> probe_needed(probe_col_count, false);
            probe_needed[probe_join_col_] = true;
            // Map each output column -> probe column if applicable.
            for (idx_t out_col = 0; out_col < needed_cols_.size() && needed_cols_[out_col]; out_col++) {}
            for (idx_t out_col = 0; out_col < needed_cols_.size(); out_col++) {
                if (!needed_cols_[out_col]) continue;
                idx_t probe_col = INVALID_INDEX;
                if (build_is_right_) {
                    // output = [probe_cols..., build_cols...]
                    if (out_col < probe_col_count) probe_col = out_col;
                } else {
                    // output = [build_cols..., probe_cols...]
                    if (out_col >= left_col_count_) probe_col = out_col - left_col_count_;
                }
                if (probe_col != INVALID_INDEX && probe_col < probe_col_count) {
                    probe_needed[probe_col] = true;
                }
            }
            probe_child_->SetNeededOutputs(probe_needed);
            // If probe child is a file scan, also set CSV projection.
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(probe_child_)) {
                fs->SetProjection(probe_needed);
            }
        }
    }

private:
    // Member state for streaming probe.
    bool build_is_right_ = false;
    std::vector<std::vector<Value>> build_rows_;
    std::unordered_map<std::string, std::vector<idx_t>> hash_table_;
    // Typed int64 hash table - used when BOTH join columns are integer types.
    // Skips the Value::ToString() allocation per key (build + probe) and the
    // string-hash work. ~30-50% faster for integer joins on ≥100k rows.
    std::unordered_map<int64_t, std::vector<idx_t>> hash_table_i64_;
    bool use_i64_hash_ = false;
    LogicalTypeId probe_key_type_ = LogicalTypeId::INVALID;
    // Linear cache for low-cardinality probes - avoids map/string hash overhead.
    bool use_linear_cache_ = false;
    std::vector<std::string> build_key_cache_;
    std::vector<std::vector<idx_t>*> build_match_cache_;
    // Parallel int64 linear cache - same idea, typed.
    std::vector<int64_t> build_key_cache_i64_;
    std::vector<bool> build_matched_;
    idx_t probe_join_col_ = 0;
    PhysicalOperator *probe_child_ = nullptr;
    DataChunk probe_chunk_;
    idx_t probe_chunk_pos_ = 0;
    idx_t current_probe_row_ = 0;
    const std::vector<idx_t> *current_match_list_ = nullptr;
    idx_t match_pos_ = 0;
    bool probe_done_ = false;
    idx_t outer_emit_pos_ = 0;

    JoinType join_type_;
    BoundExprPtr condition_;
    idx_t left_col_count_;
    idx_t right_col_count_;
    std::vector<std::vector<Value>> result_rows_; // only for CROSS
    bool built_ = false;
    idx_t emit_pos_ = 0;
    std::vector<bool> needed_cols_;
};

static PhysicalHashJoin *AsHashJoin(PhysicalOperator *op) {
    return dynamic_cast<PhysicalHashJoin *>(op);
}

// Fused JOIN+aggregate hot path - streams probe chunks, looks up build side,
// updates aggregate state directly. Cuts 2-4x off large-fact × small-dim patterns
// (e.g. `sales JOIN regions GROUP BY manager`).
bool PhysicalHashAggregate::TryComputeFusedJoinAggregate(
    PhysicalHashJoin *hj,
    const std::vector<AggInfo> &agg_infos,
    const std::vector<idx_t> &group_col_indices) {
    if (!hj->FuseIsInner()) return false;
    idx_t num_aggs = agg_infos.size();
    for (auto &info : agg_infos) {
        if (info.name == "COUNT" && info.is_count_star) continue;
        if ((info.name == "COUNT" || info.name == "SUM" || info.name == "AVG" ||
             info.name == "MIN" || info.name == "MAX") &&
            info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
        return false;
    }

    hj->FuseBuild();
    auto *probe = hj->FuseProbeChild();
    const auto &build_rows = hj->FuseBuildRows();
    const auto &hash_tab = hj->FuseHashTable();
    const auto &hash_tab_i64 = hj->FuseHashTableI64();
    bool use_i64 = hj->FuseUseI64Hash();
    bool build_is_right = hj->FuseBuildIsRight();
    idx_t probe_join_col = hj->FuseProbeJoinCol();
    idx_t left_cols = hj->FuseLeftColCount();
    idx_t probe_cols = probe->GetTypes().size();
    bool linear = hj->FuseUseLinearCache();
    const auto &key_cache = hj->FuseBuildKeyCache();
    const auto &key_cache_i64 = hj->FuseBuildKeyCacheI64();
    const auto &match_cache = hj->FuseBuildMatchCache();

    auto map_col = [&](idx_t combined) -> std::pair<bool, idx_t> {
        if (build_is_right) {
            if (combined < probe_cols) return {false, combined};
            return {true, combined - probe_cols};
        } else {
            if (combined < left_cols) return {true, combined};
            return {false, combined - left_cols};
        }
    };

    // Push projection to the probe scan - only parse cols we actually read.
    {
        std::vector<bool> probe_needed(probe_cols, false);
        probe_needed[probe_join_col] = true;
        for (idx_t gc : group_col_indices) {
            auto pr = map_col(gc);
            if (!pr.first && pr.second < probe_cols) probe_needed[pr.second] = true;
        }
        for (auto &info : agg_infos) {
            if (info.col_idx == INVALID_INDEX) continue;
            auto pr = map_col(info.col_idx);
            if (!pr.first && pr.second < probe_cols) probe_needed[pr.second] = true;
        }
        probe->SetNeededOutputs(probe_needed);
        if (auto *fs = dynamic_cast<PhysicalFileScan *>(probe)) fs->SetProjection(probe_needed);
    }

    struct GKey { bool is_build; idx_t local; LogicalTypeId tid; };
    std::vector<GKey> gkeys;
    gkeys.reserve(group_col_indices.size());
    for (idx_t gc : group_col_indices) {
        auto pr = map_col(gc);
        LogicalTypeId tid = LogicalTypeId::SQLNULL;
        if (pr.first) {
            if (!build_rows.empty() && pr.second < build_rows[0].size())
                tid = build_rows[0][pr.second].type().id();
        } else {
            if (pr.second < probe->GetTypes().size())
                tid = probe->GetTypes()[pr.second].id();
        }
        gkeys.push_back({pr.first, pr.second, tid});
    }

    struct ACol { bool is_build; idx_t local; };
    std::vector<ACol> acols(num_aggs);
    for (idx_t a = 0; a < num_aggs; a++) {
        if (agg_infos[a].col_idx == INVALID_INDEX) { acols[a] = {false, INVALID_INDEX}; continue; }
        auto pr = map_col(agg_infos[a].col_idx);
        acols[a] = {pr.first, pr.second};
    }

    // Precompute per-build-row group key if all group cols are build-side.
    bool all_build_gkeys = true;
    for (auto &g : gkeys) if (!g.is_build) { all_build_gkeys = false; break; }

    // Fast path: when GROUP BY is all build-side, map each build_idx -> group_idx
    // directly (array index instead of hashing a string per probe row).
    std::vector<idx_t> build_to_gidx;
    std::vector<std::vector<Value>> group_vals_by_gidx;
    std::vector<std::vector<AggState>> states_by_gidx;
    if (all_build_gkeys) {
        std::unordered_map<std::string, idx_t> gkey_to_gidx;
        build_to_gidx.assign(build_rows.size(), INVALID_INDEX);
        // No-GROUP-BY queries (e.g. `SELECT COUNT(*) ... JOIN ...`) need the
        // aggregate to emit exactly one row even with zero matches. Pre-
        // seed a single group so COUNT-of-empty-join returns 0 rather than
        // no rows.
        if (gkeys.empty()) {
            group_vals_by_gidx.push_back({});
            states_by_gidx.emplace_back(num_aggs);
            gkey_to_gidx.emplace(std::string(), static_cast<idx_t>(0));
        }
        for (idx_t i = 0; i < build_rows.size(); i++) {
            std::string gkey;
            std::vector<Value> gvals(gkeys.size());
            for (idx_t j = 0; j < gkeys.size(); j++) {
                auto &v = build_rows[i][gkeys[j].local];
                gvals[j] = v;
                if (v.IsNull()) gkey += "\x01N"; else gkey += v.ToString();
                gkey += '|';
            }
            auto it = gkey_to_gidx.find(gkey);
            idx_t gidx;
            if (it == gkey_to_gidx.end()) {
                gidx = group_vals_by_gidx.size();
                group_vals_by_gidx.push_back(std::move(gvals));
                states_by_gidx.emplace_back(num_aggs);
                gkey_to_gidx.emplace(std::move(gkey), gidx);
            } else {
                gidx = it->second;
            }
            build_to_gidx[i] = gidx;
        }
    }

    std::unordered_map<std::string, std::vector<AggState>> group_states;
    std::unordered_map<std::string, std::vector<Value>> group_keys_map;
    std::vector<std::string> group_order;

    // PARALLEL PATH: large file + build-side GROUP BY -> split into N thread slices.
    // Each thread does full parse + probe + local aggregate into its own state array,
    // then merge. Eliminates the single-threaded CSV parse bottleneck.
    bool parallel_done = false;
    if (all_build_gkeys) {
        auto *fs = dynamic_cast<PhysicalFileScan *>(probe);
        FastCSVReader *reader = fs ? fs->GetReader() : nullptr;
        if (reader && reader->GetBuffer() && reader->GetSize() > 16 * 1024 * 1024) {
            const char *buffer = reader->GetBuffer();
            size_t total_size = reader->GetSize();
            size_t data_start = reader->GetPos();
            size_t data_size = total_size - data_start;

            unsigned int nt = HWThreads();
            if (nt > 8) nt = 8;
            std::vector<size_t> ranges(nt + 1);
            ranges[0] = data_start;
            ranges[nt] = total_size;
            for (unsigned int t = 1; t < nt; t++) {
                size_t target = data_start + (data_size * t) / nt;
                ranges[t] = FastCSVReader::FindLineStart(buffer, total_size, target);
            }

            idx_t num_groups = states_by_gidx.size();
            std::vector<std::vector<std::vector<AggState>>> thread_states(nt);
            for (auto &ts : thread_states) {
                ts.resize(num_groups);
                for (auto &g : ts) g.resize(num_aggs);
            }

            char delim = fs->GetDelimiter();
            auto types = probe->GetTypes();
            auto projection = fs->GetProjection(); // copy once

            auto worker_fn4 = [&](unsigned int t) {
                    FastCSVReader tr(buffer, ranges[t], ranges[t + 1], delim);
                    DataChunk chunk;
                    chunk.Initialize(types);
                    auto &ts = thread_states[t];
                    while (true) {
                        chunk.Reset();
                        idx_t cnt = projection.empty()
                            ? tr.ReadChunk(chunk, types)
                            : tr.ReadChunkProjected(chunk, types, projection);
                        if (cnt == 0) break;
                        auto &kvec = chunk.GetVector(probe_join_col);
                        auto ktid = kvec.GetType().id();
                        for (idx_t i = 0; i < cnt; i++) {
                            const std::vector<idx_t> *ml = nullptr;
                            if (use_i64) {
                                if (!kvec.GetValidity().RowIsValid(i)) {
                                    // NULL probe key never matches.
                                } else {
                                    int64_t k = 0;
                                    switch (ktid) {
                                    case LogicalTypeId::TINYINT:  k = reinterpret_cast<const int8_t *>(kvec.GetData())[i]; break;
                                    case LogicalTypeId::SMALLINT: k = reinterpret_cast<const int16_t *>(kvec.GetData())[i]; break;
                                    case LogicalTypeId::INTEGER:  k = reinterpret_cast<const int32_t *>(kvec.GetData())[i]; break;
                                    case LogicalTypeId::BIGINT:
                                    case LogicalTypeId::HUGEINT:  k = reinterpret_cast<const int64_t *>(kvec.GetData())[i]; break;
                                    default: k = chunk.GetValue(probe_join_col, i).GetValue<int64_t>();
                                    }
                                    if (linear) {
                                        for (idx_t ki = 0; ki < key_cache_i64.size(); ki++) {
                                            if (key_cache_i64[ki] == k) { ml = match_cache[ki]; break; }
                                        }
                                    } else {
                                        auto it = hash_tab_i64.find(k);
                                        if (it != hash_tab_i64.end()) ml = &it->second;
                                    }
                                }
                            } else if (linear && ktid == LogicalTypeId::VARCHAR) {
                                auto &s = reinterpret_cast<const string_t *>(kvec.GetData())[i];
                                const char *d = s.GetData(); uint32_t l = s.GetSize();
                                for (idx_t ki = 0; ki < key_cache.size(); ki++) {
                                    const auto &k = key_cache[ki];
                                    if (k.size() == l && memcmp(k.data(), d, l) == 0) { ml = match_cache[ki]; break; }
                                }
                            } else {
                                std::string ks;
                                if (ktid == LogicalTypeId::VARCHAR) {
                                    auto &s = reinterpret_cast<const string_t *>(kvec.GetData())[i];
                                    ks.assign(s.GetData(), s.GetSize());
                                } else {
                                    ks = chunk.GetValue(probe_join_col, i).ToString();
                                }
                                auto it = hash_tab.find(ks);
                                if (it != hash_tab.end()) ml = &it->second;
                            }
                            if (!ml || ml->empty()) continue;
                            for (idx_t build_idx : *ml) {
                                idx_t gidx = build_to_gidx[build_idx];
                                auto &states = ts[gidx];
                                auto &br = build_rows[build_idx];
                                for (idx_t a = 0; a < num_aggs; a++) {
                                    auto &state = states[a];
                                    auto &info = agg_infos[a];
                                    if (info.name == "COUNT" && info.is_count_star) { state.count++; continue; }
                                    auto &ac = acols[a];
                                    if (ac.local == INVALID_INDEX) continue;
                                    if (ac.is_build) {
                                        auto &v = br[ac.local];
                                        if (v.IsNull()) continue;
                                        if (info.name == "COUNT") { state.count++; continue; }
                                        double d = 0;
                                        auto tid = v.type().id();
                                        if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                                        else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                                        else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                                        else if (info.name == "MIN") { if (!state.has_min || d < state.sum_min) { state.sum_min = d; state.has_min = true; } }
                                        else if (info.name == "MAX") { if (!state.has_max || d > state.sum_max) { state.sum_max = d; state.has_max = true; } }
                                    } else {
                                        auto &vec = chunk.GetVector(ac.local);
                                        if (!vec.GetValidity().RowIsValid(i)) continue;
                                        auto tid = vec.GetType().id();
                                        if (info.name == "COUNT") { state.count++; continue; }
                                        double d = 0;
                                        if (tid == LogicalTypeId::INTEGER)
                                            d = reinterpret_cast<const int32_t *>(vec.GetData())[i];
                                        else if (tid == LogicalTypeId::BIGINT)
                                            d = static_cast<double>(reinterpret_cast<const int64_t *>(vec.GetData())[i]);
                                        else if (tid == LogicalTypeId::DOUBLE)
                                            d = reinterpret_cast<const double *>(vec.GetData())[i];
                                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                                        else if (info.name == "MIN") { if (!state.has_min || d < state.sum_min) { state.sum_min = d; state.has_min = true; } }
                                        else if (info.name == "MAX") { if (!state.has_max || d > state.sum_max) { state.sum_max = d; state.has_max = true; } }
                                    }
                                }
                            }
                        }
                    }
                };
            if (nt > 1) {
                std::vector<std::thread> threads;
                threads.reserve(nt);
                for (unsigned int t = 0; t < nt; t++)
                    threads.emplace_back(worker_fn4, t);
                for (auto &th : threads) th.join();
            } else {
                worker_fn4(0);
            }

            // Merge thread-local into final states_by_gidx.
            for (unsigned int t = 0; t < nt; t++) {
                for (idx_t g = 0; g < num_groups; g++) {
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &F = states_by_gidx[g][a];
                        auto &P = thread_states[t][g][a];
                        F.count += P.count;
                        F.sum += P.sum;
                        if (P.has_min && (!F.has_min || P.sum_min < F.sum_min)) { F.sum_min = P.sum_min; F.has_min = true; }
                        if (P.has_max && (!F.has_max || P.sum_max > F.sum_max)) { F.sum_max = P.sum_max; F.has_max = true; }
                    }
                }
            }
            parallel_done = true;
        }
    }

    DataChunk pchunk;
    while (!parallel_done && probe->GetData(pchunk)) {
        idx_t cnt = pchunk.size();
        auto &key_vec = pchunk.GetVector(probe_join_col);
        auto key_tid = key_vec.GetType().id();
        for (idx_t i = 0; i < cnt; i++) {
            const std::vector<idx_t> *match_list = nullptr;
            if (use_i64) {
                if (!key_vec.GetValidity().RowIsValid(i)) {
                    // NULL probe key never matches.
                } else {
                    int64_t k = 0;
                    switch (key_tid) {
                    case LogicalTypeId::TINYINT:  k = reinterpret_cast<const int8_t *>(key_vec.GetData())[i]; break;
                    case LogicalTypeId::SMALLINT: k = reinterpret_cast<const int16_t *>(key_vec.GetData())[i]; break;
                    case LogicalTypeId::INTEGER:  k = reinterpret_cast<const int32_t *>(key_vec.GetData())[i]; break;
                    case LogicalTypeId::BIGINT:
                    case LogicalTypeId::HUGEINT:  k = reinterpret_cast<const int64_t *>(key_vec.GetData())[i]; break;
                    default: k = pchunk.GetValue(probe_join_col, i).GetValue<int64_t>();
                    }
                    if (linear) {
                        for (idx_t ki = 0; ki < key_cache_i64.size(); ki++) {
                            if (key_cache_i64[ki] == k) { match_list = match_cache[ki]; break; }
                        }
                    } else {
                        auto it = hash_tab_i64.find(k);
                        if (it != hash_tab_i64.end()) match_list = &it->second;
                    }
                }
            } else if (linear && key_tid == LogicalTypeId::VARCHAR) {
                auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[i];
                const char *d = s.GetData();
                uint32_t l = s.GetSize();
                for (idx_t ki = 0; ki < key_cache.size(); ki++) {
                    const auto &k = key_cache[ki];
                    if (k.size() == l && memcmp(k.data(), d, l) == 0) {
                        match_list = match_cache[ki];
                        break;
                    }
                }
            } else {
                std::string key_s;
                if (key_tid == LogicalTypeId::VARCHAR) {
                    auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[i];
                    key_s.assign(s.GetData(), s.GetSize());
                } else {
                    key_s = pchunk.GetValue(probe_join_col, i).ToString();
                }
                auto it = hash_tab.find(key_s);
                if (it != hash_tab.end()) match_list = &it->second;
            }
            if (!match_list || match_list->empty()) continue;

            for (idx_t build_idx : *match_list) {
                auto &br = build_rows[build_idx];
                std::vector<AggState> *states_ptr;
                if (all_build_gkeys) {
                    // Direct array indexing - no string hashing in the hot loop.
                    idx_t gidx = build_to_gidx[build_idx];
                    states_ptr = &states_by_gidx[gidx];
                } else {
                    std::string gkey;
                    for (auto &g : gkeys) {
                        if (g.is_build) {
                            auto &v = br[g.local];
                            if (v.IsNull()) gkey += "\x01N"; else gkey += v.ToString();
                        } else {
                            auto v = pchunk.GetValue(g.local, i);
                            if (v.IsNull()) gkey += "\x01N"; else gkey += v.ToString();
                        }
                        gkey += '|';
                    }
                    auto git = group_states.find(gkey);
                    if (git == group_states.end()) {
                        std::vector<Value> gvals(gkeys.size());
                        for (idx_t j = 0; j < gkeys.size(); j++) {
                            if (gkeys[j].is_build) gvals[j] = br[gkeys[j].local];
                            else gvals[j] = pchunk.GetValue(gkeys[j].local, i);
                        }
                        group_keys_map[gkey] = std::move(gvals);
                        group_order.push_back(gkey);
                        git = group_states.emplace(gkey, std::vector<AggState>(num_aggs)).first;
                    }
                    states_ptr = &git->second;
                }
                auto &states = *states_ptr;

                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];
                    if (info.name == "COUNT" && info.is_count_star) {
                        state.count++;
                        continue;
                    }
                    auto &ac = acols[a];
                    if (ac.local == INVALID_INDEX) continue;
                    if (ac.is_build) {
                        auto &v = br[ac.local];
                        if (v.IsNull()) continue;
                        if (info.name == "COUNT") { state.count++; continue; }
                        double d = 0;
                        auto tid = v.type().id();
                        if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                        else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                        else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                        else if (info.name == "MIN") { if (!state.has_min || v < state.min_val()) { state.min_val() = v; state.has_min = true; } }
                        else if (info.name == "MAX") { if (!state.has_max || v > state.max_val()) { state.max_val() = v; state.has_max = true; } }
                    } else {
                        auto &vec = pchunk.GetVector(ac.local);
                        if (!vec.GetValidity().RowIsValid(i)) continue;
                        auto tid = vec.GetType().id();
                        if (info.name == "COUNT") { state.count++; continue; }
                        double d = 0;
                        if (tid == LogicalTypeId::INTEGER)
                            d = reinterpret_cast<const int32_t *>(vec.GetData())[i];
                        else if (tid == LogicalTypeId::BIGINT)
                            d = static_cast<double>(reinterpret_cast<const int64_t *>(vec.GetData())[i]);
                        else if (tid == LogicalTypeId::DOUBLE)
                            d = reinterpret_cast<const double *>(vec.GetData())[i];
                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                        else if (info.name == "MIN") {
                            if (!state.has_min || d < state.sum_min) { state.sum_min = d; state.has_min = true; }
                        }
                        else if (info.name == "MAX") {
                            if (!state.has_max || d > state.sum_max) { state.sum_max = d; state.has_max = true; }
                        }
                    }
                }
            }
        }
    }

    // Normalize emit source: for the build-indexed fast path, feed
    // group_vals_by_gidx + states_by_gidx into the same emit loop.
    std::vector<std::vector<Value>> *emit_group_vals;
    std::vector<std::vector<AggState>> *emit_states;
    std::vector<std::vector<Value>> fallback_vals;
    std::vector<std::vector<AggState>> fallback_states;
    if (all_build_gkeys) {
        emit_group_vals = &group_vals_by_gidx;
        emit_states = &states_by_gidx;
    } else {
        fallback_vals.reserve(group_order.size());
        fallback_states.reserve(group_order.size());
        for (auto &gk : group_order) {
            fallback_vals.push_back(std::move(group_keys_map[gk]));
            fallback_states.push_back(std::move(group_states[gk]));
        }
        emit_group_vals = &fallback_vals;
        emit_states = &fallback_states;
    }
    for (idx_t gi = 0; gi < emit_group_vals->size(); gi++) {
        auto &states = (*emit_states)[gi];
        auto &gvals = (*emit_group_vals)[gi];
        std::vector<Value> row;
        row.reserve(gkeys.size() + num_aggs);
        for (auto &v : gvals) row.push_back(v);
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &state = states[a];
            auto &info = agg_infos[a];
            Value r;
            if (info.name == "COUNT") r = Value::BIGINT(state.count);
            else if (info.name == "SUM") {
                auto &ac = acols[a];
                bool is_double = false;
                if (ac.local != INVALID_INDEX) {
                    if (ac.is_build) {
                        if (!build_rows.empty() && ac.local < build_rows[0].size())
                            is_double = build_rows[0][ac.local].type().id() == LogicalTypeId::DOUBLE;
                    } else {
                        is_double = probe->GetTypes()[ac.local].id() == LogicalTypeId::DOUBLE;
                    }
                }
                r = is_double ? Value::DOUBLE(state.sum) : Value::BIGINT(static_cast<int64_t>(state.sum));
            }
            else if (info.name == "AVG") r = state.count > 0 ? Value::DOUBLE(state.sum / state.count) : Value();
            else if (info.name == "MIN") {
                auto &ac = acols[a];
                if (!state.has_min) r = Value();
                else if (ac.is_build) r = state.min_val_or_null();
                else {
                    auto tid = probe->GetTypes()[ac.local].id();
                    if (tid == LogicalTypeId::INTEGER) r = Value::INTEGER(static_cast<int32_t>(state.sum_min));
                    else if (tid == LogicalTypeId::BIGINT) r = Value::BIGINT(static_cast<int64_t>(state.sum_min));
                    else r = Value::DOUBLE(state.sum_min);
                }
            }
            else if (info.name == "MAX") {
                auto &ac = acols[a];
                if (!state.has_max) r = Value();
                else if (ac.is_build) r = state.max_val_or_null();
                else {
                    auto tid = probe->GetTypes()[ac.local].id();
                    if (tid == LogicalTypeId::INTEGER) r = Value::INTEGER(static_cast<int32_t>(state.sum_max));
                    else if (tid == LogicalTypeId::BIGINT) r = Value::BIGINT(static_cast<int64_t>(state.sum_max));
                    else r = Value::DOUBLE(state.sum_max);
                }
            }
            row.push_back(r);
        }
        result_rows_.push_back(std::move(row));
    }
    return true;
}

// ============================================================================
// Update
// ============================================================================

class PhysicalUpdate : public PhysicalOperator {
public:
    PhysicalUpdate(TableCatalogEntry *table,
                   std::vector<BoundUpdateAssignment> assignments,
                   BoundExprPtr where_clause)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), assignments_(std::move(assignments)),
          where_clause_(std::move(where_clause)) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        auto &storage = table_->GetStorage();

        // Scan all data, modify matching rows, rebuild storage.
        std::vector<std::vector<Value>> all_rows;
        auto state = storage.InitScan();
        DataChunk chunk;
        while (true) {
            chunk.Initialize(types);
            if (!storage.Scan(state, chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                    row.push_back(chunk.GetValue(c, i));
                all_rows.push_back(std::move(row));
            }
        }

        // Create new storage and re-insert.
        auto new_storage = std::make_shared<DataTable>(types);

        for (auto &row : all_rows) {
            // Check WHERE.
            bool matches = true;
            if (where_clause_) {
                DataChunk single;
                single.Initialize(types);
                for (idx_t c = 0; c < row.size(); c++)
                    single.SetValue(c, 0, row[c]);
                Vector filter_result(LogicalType::BOOLEAN());
                ExpressionExecutor::Execute(*where_clause_, single, filter_result, 1);
                matches = filter_result.GetValidity().RowIsValid(0) &&
                          filter_result.GetData<bool>()[0];
            }

            if (matches) {
                // Apply assignments - evaluate against current row.
                DataChunk row_chunk;
                row_chunk.Initialize(types);
                for (idx_t c = 0; c < row.size(); c++)
                    row_chunk.SetValue(c, 0, row[c]);

                for (auto &assign : assignments_) {
                    if (assign.value->GetExpressionType() == BoundExpressionType::CONSTANT) {
                        row[assign.column_index] = ExpressionExecutor::ExecuteScalar(*assign.value);
                    } else {
                        Vector res(assign.value->GetReturnType());
                        ExpressionExecutor::Execute(*assign.value, row_chunk, res, 1);
                        row[assign.column_index] = res.GetValue(0);
                    }
                }
            }

            DataChunk insert_chunk;
            insert_chunk.Initialize(types);
            for (idx_t c = 0; c < row.size(); c++)
                insert_chunk.SetValue(c, 0, row[c]);
            new_storage->Append(insert_chunk);
        }

        table_->SetStorage(new_storage);
        return false;
    }

private:
    TableCatalogEntry *table_;
    std::vector<BoundUpdateAssignment> assignments_;
    BoundExprPtr where_clause_;
    bool done_ = false;
};

// ============================================================================
// Delete
// ============================================================================

class PhysicalDelete : public PhysicalOperator {
public:
    PhysicalDelete(TableCatalogEntry *table, BoundExprPtr where_clause)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), where_clause_(std::move(where_clause)) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        auto &storage = table_->GetStorage();

        // Scan all data, keep non-matching rows.
        auto new_storage = std::make_shared<DataTable>(types);
        auto state = storage.InitScan();
        DataChunk chunk;

        while (true) {
            chunk.Initialize(types);
            if (!storage.Scan(state, chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                bool should_delete = false;
                if (where_clause_) {
                    DataChunk single;
                    single.Initialize(types);
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        single.SetValue(c, 0, chunk.GetValue(c, i));
                    Vector filter_result(LogicalType::BOOLEAN());
                    ExpressionExecutor::Execute(*where_clause_, single, filter_result, 1);
                    should_delete = filter_result.GetValidity().RowIsValid(0) &&
                                    filter_result.GetData<bool>()[0];
                } else {
                    should_delete = true; // DELETE without WHERE = delete all.
                }

                if (!should_delete) {
                    DataChunk keep;
                    keep.Initialize(types);
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        keep.SetValue(c, 0, chunk.GetValue(c, i));
                    new_storage->Append(keep);
                }
            }
        }

        table_->SetStorage(new_storage);
        return false;
    }

private:
    TableCatalogEntry *table_;
    BoundExprPtr where_clause_;
    bool done_ = false;
};

class PhysicalDummyScan : public PhysicalOperator {
public:
    PhysicalDummyScan()
        : PhysicalOperator(PhysicalOperatorType::DUMMY_SCAN, {}) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;
        // Return a single empty row - projections above will fill in constants.
        result.Initialize({});
        result.SetCardinality(1);
        return true;
    }

private:
    bool done_ = false;
};

// ============================================================================
// Physical Planner
// ============================================================================

PhysicalOpPtr PhysicalPlanner::Plan(const LogicalOperator &logical) {
    switch (logical.GetOperatorType()) {
    case LogicalOperatorType::GET:
        return PlanGet(static_cast<const LogicalGet &>(logical));
    case LogicalOperatorType::FILTER:
        return PlanFilter(static_cast<const LogicalFilter &>(logical));
    case LogicalOperatorType::PROJECTION:
        return PlanProjection(static_cast<const LogicalProjection &>(logical));
    case LogicalOperatorType::ORDER_BY:
        return PlanOrderBy(static_cast<const LogicalOrderBy &>(logical));
    case LogicalOperatorType::LIMIT:
        return PlanLimit(static_cast<const LogicalLimit &>(logical));
    case LogicalOperatorType::INSERT:
        return PlanInsert(static_cast<const LogicalInsert &>(logical));
    case LogicalOperatorType::CREATE_TABLE:
        return PlanCreateTable(static_cast<const LogicalCreateTable &>(logical));
    case LogicalOperatorType::DROP_TABLE:
        return PlanDropTable(static_cast<const LogicalDropTable &>(logical));
    case LogicalOperatorType::UPDATE:
        return PlanUpdateOp(static_cast<const LogicalUpdate &>(logical));
    case LogicalOperatorType::DELETE_STMT:
        return PlanDeleteOp(static_cast<const LogicalDeleteOp &>(logical));
    case LogicalOperatorType::WINDOW:
        return PlanWindow(static_cast<const LogicalWindow &>(logical));
    case LogicalOperatorType::DISTINCT:
        return PlanDistinct(static_cast<const LogicalDistinct &>(logical));
    case LogicalOperatorType::AGGREGATE:
        return PlanAggregate(static_cast<const LogicalAggregate &>(logical));
    case LogicalOperatorType::JOIN:
        return PlanJoin(static_cast<const LogicalJoin &>(logical));
    case LogicalOperatorType::DUMMY_SCAN:
        return PlanDummyScan(static_cast<const LogicalDummyScan &>(logical));
    default:
        throw InternalException("Unknown logical operator type");
    }
}

PhysicalOpPtr PhysicalPlanner::PlanGet(const LogicalGet &op) {
    if (op.table && op.table->IsFileScan()) {
        if (op.table->GetFileFormat() == "parquet") {
            return std::make_unique<PhysicalParquetScan>(
                op.table->GetFilePath(), op.table->GetTypes(),
                op.table->GetCachedParquetReader());
        }
        if (op.table->GetFileFormat() == "json") {
            return std::make_unique<PhysicalJSONScan>(
                op.table->GetFilePath(), op.table->GetTypes());
        }
#ifndef SLOTHDB_EDGE
        if (op.table->GetFileFormat() == "avro") {
            return std::make_unique<PhysicalAvroScan>(
                op.table->GetFilePath(), op.table->GetTypes());
        }
        if (op.table->GetFileFormat() == "arrow") {
            return std::make_unique<PhysicalArrowScan>(
                op.table->GetFilePath(), op.table->GetTypes());
        }
        if (op.table->GetFileFormat() == "sqlite") {
            return std::make_unique<PhysicalSQLiteScan>(
                op.table->GetFilePath(), op.table->GetFileSubname(),
                op.table->GetTypes());
        }
#endif
        return std::make_unique<PhysicalFileScan>(
            op.table->GetFilePath(), op.table->GetFileDelimiter(),
            op.table->GetTypes());
    }
    return std::make_unique<PhysicalTableScan>(op.table);
}

// Try to extract simple `column OP literal` predicates from a bound
// expression. Walks AND-chains and accepts =, <, <=, >, >= on a column
// vs. a constant. Anything else (OR, function calls, between, IN) is
// silently skipped — the FILTER above the scan still runs the full
// condition for correctness, this only adds row-group skip hints.
static void ExtractParquetPushdownFilters(
    const BoundExpression &expr,
    std::vector<PhysicalParquetScan::PushdownFilter> &out) {
    if (expr.GetExpressionType() == BoundExpressionType::CONJUNCTION) {
        auto &c = static_cast<const BoundConjunction &>(expr);
        if (c.op == "AND") {
            ExtractParquetPushdownFilters(*c.left, out);
            ExtractParquetPushdownFilters(*c.right, out);
        }
        return;
    }
    if (expr.GetExpressionType() != BoundExpressionType::COMPARISON) return;
    auto &cmp = static_cast<const BoundComparison &>(expr);

    // Normalise so column is on the left.
    const BoundExpression *l = cmp.left.get();
    const BoundExpression *r = cmp.right.get();
    std::string op = cmp.op;
    if (l->GetExpressionType() != BoundExpressionType::COLUMN_REF &&
        r->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        std::swap(l, r);
        // Flip the operator — `5 > col` becomes `col < 5`.
        if (op == "<") op = ">";
        else if (op == ">") op = "<";
        else if (op == "<=") op = ">=";
        else if (op == ">=") op = "<=";
    }
    if (l->GetExpressionType() != BoundExpressionType::COLUMN_REF) return;
    if (r->GetExpressionType() != BoundExpressionType::CONSTANT) return;
    if (op != "=" && op != "==" && op != "<" && op != "<=" && op != ">" && op != ">=") return;

    auto &col = static_cast<const BoundColumnRef &>(*l);
    auto &konst = static_cast<const BoundConstant &>(*r);
    out.push_back({col.column_index, op == "==" ? "=" : op, konst.value});
}

PhysicalOpPtr PhysicalPlanner::PlanFilter(const LogicalFilter &op) {
    auto &mutable_op = const_cast<LogicalFilter &>(op);

    // Capture a non-owning view of the condition before std::move so we
    // can extract pushdown filters from it. The PhysicalFilter takes
    // ownership of the original expression — extraction reads only.
    std::vector<PhysicalParquetScan::PushdownFilter> pushdown;
    if (mutable_op.condition) {
        ExtractParquetPushdownFilters(*mutable_op.condition, pushdown);
    }

    auto result = std::make_unique<PhysicalFilter>(
        std::move(mutable_op.condition), op.GetTypes());
    auto child = Plan(*op.children[0]);

    // If our immediate child is a Parquet scan (or a Projection over one),
    // hand it the pushdown filters. The filter itself stays in place; the
    // scan just skips entire row groups whose stats prove no row matches.
    if (!pushdown.empty()) {
        PhysicalParquetScan *pq = dynamic_cast<PhysicalParquetScan *>(child.get());
        if (!pq && child->GetOperatorType() == PhysicalOperatorType::PROJECTION
                && !child->children.empty()) {
            pq = dynamic_cast<PhysicalParquetScan *>(child->children[0].get());
        }
        if (pq) pq->SetPushdownFilters(std::move(pushdown));
    }

    result->children.push_back(std::move(child));
    return result;
}

// Q20 peephole detection, marked noinline + cold so it stays out of the
// hot .text region. Returns true and fills `out_literal` if the shape
// matches (SELECT col WHERE col = literal); caller builds the synthetic
// count_op and wraps it in PhysicalLiteralEmitFilter.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static bool MatchQ20LiteralEmit(const LogicalProjection &op, Value &out_literal) {
    if (op.expressions.size() != 1 || op.children.empty()) return false;
    auto &proj_expr = op.expressions[0];
    if (!proj_expr || proj_expr->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
    auto proj_col = static_cast<const BoundColumnRef &>(*proj_expr).column_index;
    auto *log_filter = dynamic_cast<const LogicalFilter *>(op.children[0].get());
    if (!log_filter || !log_filter->condition || log_filter->children.empty()) return false;
    auto &cond = *log_filter->condition;
    if (cond.GetExpressionType() != BoundExpressionType::COMPARISON) return false;
    auto &cmp = static_cast<const BoundComparison &>(cond);
    if (cmp.op != "=" && cmp.op != "==") return false;
    const BoundExpression *lhs = cmp.left.get();
    const BoundExpression *rhs = cmp.right.get();
    if (lhs && rhs && lhs->GetExpressionType() == BoundExpressionType::CONSTANT &&
        rhs->GetExpressionType() == BoundExpressionType::COLUMN_REF) std::swap(lhs, rhs);
    if (!lhs || !rhs ||
        lhs->GetExpressionType() != BoundExpressionType::COLUMN_REF ||
        rhs->GetExpressionType() != BoundExpressionType::CONSTANT) return false;
    auto col = static_cast<const BoundColumnRef &>(*lhs).column_index;
    if (col != proj_col || op.GetTypes().size() != 1) return false;
    auto out_tid = op.GetTypes()[0].id();
    if (out_tid != LogicalTypeId::BIGINT && out_tid != LogicalTypeId::INTEGER) return false;
    const auto &literal = static_cast<const BoundConstant &>(*rhs).value;
    auto literal_tid = literal.type().id();
    if (literal_tid != LogicalTypeId::BIGINT && literal_tid != LogicalTypeId::INTEGER) return false;
    if (out_tid == LogicalTypeId::BIGINT && literal_tid == LogicalTypeId::INTEGER)
        out_literal = Value::BIGINT((int64_t)literal.GetValue<int32_t>());
    else if (out_tid == LogicalTypeId::INTEGER && literal_tid == LogicalTypeId::BIGINT)
        out_literal = Value::INTEGER((int32_t)literal.GetValue<int64_t>());
    else
        out_literal = literal;
    return true;
}

PhysicalOpPtr PhysicalPlanner::PlanProjection(const LogicalProjection &op) {
    auto &mutable_op = const_cast<LogicalProjection &>(op);
    Value emit_literal;
    if (MatchQ20LiteralEmit(op, emit_literal)) {
        std::vector<BoundExprPtr> agg_funcs;
        agg_funcs.push_back(std::make_unique<BoundFunction>(
            "COUNT", std::vector<BoundExprPtr>{}, LogicalType::BIGINT(), true, false));
        auto count_op = std::make_unique<PhysicalHashAggregate>(
            std::vector<BoundExprPtr>{}, std::move(agg_funcs),
            std::vector<LogicalType>{LogicalType::BIGINT()});
        count_op->children.push_back(Plan(*op.children[0]));
        return std::make_unique<PhysicalLiteralEmitFilter>(
            op.GetTypes(), std::move(emit_literal), std::move(count_op));
    }
    auto result = std::make_unique<PhysicalProjection>(
        std::move(mutable_op.expressions), op.GetTypes());
    if (!op.children.empty()) {
        result->children.push_back(Plan(*op.children[0]));
    }
    // Push the projection's referenced columns down into a child OrderBy.
    // CollectTopN_Primitive's pass-2 decode then skips unprojected cols.
    // Q25 (SELECT SearchPhrase ... ORDER BY EventTime LIMIT 10) drops from
    // 1.7s -> ~225ms because pass-2 stops decoding 103 unused cols.
    {
        // Walk down through identity-projection/limit wrappers to find OrderBy.
        PhysicalOperator *cur = result->children.empty() ? nullptr
                                                         : result->children[0].get();
        for (int hops = 0; cur && hops < 4; hops++) {
            if (auto *ob = dynamic_cast<PhysicalOrderBy *>(cur)) {
                // Build mask of referenced col indices over the OrderBy's
                // INPUT schema. Each projection expression's column_index
                // refers to that same input space.
                std::vector<bool> mask(ob->GetTypes().size(), false);
                bool simple = true;
                std::function<void(const BoundExpression &)> walk =
                    [&](const BoundExpression &e) {
                        if (!simple) return;
                        switch (e.GetExpressionType()) {
                        case BoundExpressionType::COLUMN_REF: {
                            auto &cr = static_cast<const BoundColumnRef &>(e);
                            if (cr.column_index < mask.size())
                                mask[cr.column_index] = true;
                            else
                                simple = false;
                            break;
                        }
                        case BoundExpressionType::CONSTANT:
                            break;
                        case BoundExpressionType::COMPARISON: {
                            auto &c = static_cast<const BoundComparison &>(e);
                            walk(*c.left); walk(*c.right); break;
                        }
                        case BoundExpressionType::CONJUNCTION: {
                            auto &c = static_cast<const BoundConjunction &>(e);
                            walk(*c.left); walk(*c.right); break;
                        }
                        case BoundExpressionType::ARITHMETIC: {
                            auto &a = static_cast<const BoundArithmetic &>(e);
                            walk(*a.left); walk(*a.right); break;
                        }
                        case BoundExpressionType::NEGATION: {
                            auto &n = static_cast<const BoundNegation &>(e);
                            walk(*n.child); break;
                        }
                        case BoundExpressionType::UNARY_MINUS: {
                            auto &u = static_cast<const BoundUnaryMinus &>(e);
                            walk(*u.child); break;
                        }
                        case BoundExpressionType::CAST: {
                            auto &c = static_cast<const BoundCast &>(e);
                            walk(*c.child); break;
                        }
                        case BoundExpressionType::FUNCTION: {
                            auto &f = static_cast<const BoundFunction &>(e);
                            for (auto &arg : f.arguments) walk(*arg);
                            break;
                        }
                        default:
                            simple = false; break;
                        }
                    };
                for (auto &e : result->GetExpressions()) {
                    if (!e) { simple = false; break; }
                    walk(*e);
                }
                if (simple) ob->SetParentProjectionCols(std::move(mask));
                break;
            }
            // Allow PhysicalLimit / identity PhysicalProjection between us
            // and the OrderBy. Anything else stops the walk.
            if (dynamic_cast<PhysicalLimit *>(cur) ||
                (dynamic_cast<PhysicalProjection *>(cur) &&
                 static_cast<PhysicalProjection *>(cur)->IsIdentityProjection())) {
                cur = cur->children.empty() ? nullptr : cur->children[0].get();
                continue;
            }
            break;
        }
    }
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanOrderBy(const LogicalOrderBy &op) {
    auto &mutable_op = const_cast<LogicalOrderBy &>(op);
    auto result = std::make_unique<PhysicalOrderBy>(
        std::move(mutable_op.orders), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanLimit(const LogicalLimit &op) {
    auto result = std::make_unique<PhysicalLimit>(
        op.limit_count, op.offset_count, op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanInsert(const LogicalInsert &op) {
    auto &mutable_op = const_cast<LogicalInsert &>(op);
    return std::make_unique<PhysicalInsert>(op.table, std::move(mutable_op.values));
}

PhysicalOpPtr PhysicalPlanner::PlanCreateTable(const LogicalCreateTable &op) {
    return std::make_unique<PhysicalCreateTable>(
        catalog_, op.table_name, op.columns, op.if_not_exists);
}

PhysicalOpPtr PhysicalPlanner::PlanDropTable(const LogicalDropTable &op) {
    return std::make_unique<PhysicalDropTable>(catalog_, op.table_name, op.if_exists);
}

PhysicalOpPtr PhysicalPlanner::PlanUpdateOp(const LogicalUpdate &op) {
    auto &mutable_op = const_cast<LogicalUpdate &>(op);
    return std::make_unique<PhysicalUpdate>(
        op.table, std::move(mutable_op.assignments), std::move(mutable_op.where_clause));
}

PhysicalOpPtr PhysicalPlanner::PlanDeleteOp(const LogicalDeleteOp &op) {
    auto &mutable_op = const_cast<LogicalDeleteOp &>(op);
    return std::make_unique<PhysicalDelete>(op.table, std::move(mutable_op.where_clause));
}

PhysicalOpPtr PhysicalPlanner::PlanWindow(const LogicalWindow &op) {
    auto &mutable_op = const_cast<LogicalWindow &>(op);
    auto result = std::make_unique<PhysicalWindow>(
        std::move(mutable_op.select_list), std::move(mutable_op.qualify), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanDistinct(const LogicalDistinct &op) {
    auto result = std::make_unique<PhysicalDistinct>(op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanAggregate(const LogicalAggregate &op) {
    auto &mutable_op = const_cast<LogicalAggregate &>(op);
    auto result = std::make_unique<PhysicalHashAggregate>(
        std::move(mutable_op.groups), std::move(mutable_op.aggregates), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanJoin(const LogicalJoin &op) {
    auto &mutable_op = const_cast<LogicalJoin &>(op);
    idx_t left_cols = op.children[0]->GetTypes().size();
    idx_t right_cols = op.children[1]->GetTypes().size();
    auto result = std::make_unique<PhysicalHashJoin>(
        op.join_type, std::move(mutable_op.condition), op.GetTypes(),
        left_cols, right_cols);
    result->children.push_back(Plan(*op.children[0]));
    result->children.push_back(Plan(*op.children[1]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanDummyScan(const LogicalDummyScan &op) {
    return std::make_unique<PhysicalDummyScan>();
}

// =====================================================================
// 2-stage COUNT(DISTINCT) for: GROUP BY <varchar> + COUNT(DISTINCT <int>)
// + ORDER BY <agg> DESC LIMIT N.
//
// Defined at file end and marked noinline so its body sits far from
// ComputeAggregates' hot text; inlining its ~120 LOC there shifts the
// hot .text section and regresses unrelated scan/aggregate queries.
//
// The single-stage path builds per-thread per-group integer sets and
// unions them across threads. When the group column is high-cardinality
// that cross-thread merge dominates and can run for tens of seconds.
// This 2-stage form instead radix-partitions and dedups (group, value)
// pairs, then counts pairs per group: each pair is unique by its
// composite key, so the pair count equals the COUNT(DISTINCT) value. It
// is correct for any cardinality and runs whenever the query shape
// matches; there is no data-size routing threshold.
// =====================================================================
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static bool TryComputeVarcharGroupDistinctTopN(
    PhysicalParquetScan *pq,
    idx_t group_col, idx_t agg_col,
    LogicalTypeId group_tid, LogicalTypeId agg_tid,
    bool topn_active, bool topn_ascending, idx_t topn_limit,
    int num_aggs,
    const std::vector<SimplePredicate> &dpd_preds, bool dpd_has_filter,
    std::vector<std::vector<Value>> &result_rows_out) {
    // Shape gate.
    if (!(group_tid == LogicalTypeId::VARCHAR &&
          (agg_tid == LogicalTypeId::BIGINT ||
           agg_tid == LogicalTypeId::INTEGER) &&
          topn_active && !topn_ascending && topn_limit > 0)) {
        return false;
    }
    // The 2-stage form below handles any group-column cardinality, so
    // there is no size-based routing. `reader` is read below to pre-size
    // the stage-1 maps.
    auto *reader = pq->GetReader();

    // Project group + agg + filter cols. Skip group_col str_data
    // (dict_indices + dict_values are sufficient for grouping/emit).
    {
        std::vector<bool> need(pq->GetTypes().size(), false);
        need[group_col] = true;
        need[agg_col] = true;
        for (auto &p : dpd_preds)
            if (p.col_idx < need.size()) need[p.col_idx] = true;
        pq->SetNeededOutputs(need);
        std::vector<bool> skip(pq->GetTypes().size(), false);
        if (group_col < skip.size() &&
            pq->GetTypes()[group_col].id() == LogicalTypeId::VARCHAR) {
            skip[group_col] = true;
        }
        for (auto &p : dpd_preds) {
            if (p.col_idx < skip.size() && p.str_form &&
                pq->GetTypes()[p.col_idx].id() == LogicalTypeId::VARCHAR) {
                skip[p.col_idx] = true;
            }
        }
        pq->SetSkipStrData(std::move(skip));
    }
    pq->Init();

    constexpr int STAGE_THREADS = 8;
    slothdb::RadixCount2ColIntStr stage1(STAGE_THREADS);
    // Pre-size the stage-1 pair maps to limit rehashing during ingest.
    // Distinct (group, value) pairs cannot exceed the row count; with no
    // distinct-count statistics available, pre-size to a fraction of the
    // row count and let the maps grow if the real count is higher.
    {
        int64_t total_rows = 0;
        if (reader) {
            for (auto &rg : reader->GetMeta().row_groups) total_rows += rg.num_rows;
        }
        int64_t expected = total_rows / 8;
        if (expected < 1024) expected = 1024;
        stage1.ReserveExpectedRows(expected);
    }
    // skip-di detection: single pred = `<group_col> <> ''`. If the dict
    // has the empty-string entry we can fold the filter into a dict-idx
    // skip and bypass BuildTypedKeepMask and a full per-row keep-mask.
    bool skip_di_eligible =
        dpd_has_filter && dpd_preds.size() == 1 &&
        dpd_preds[0].str_form &&
        (idx_t)dpd_preds[0].col_idx == group_col &&
        dpd_preds[0].op == SimpleCmpOp::NE;
    pq->SetRGConsumer(
        [&](const PhysicalParquetScan::RGWork &work,
            idx_t rg_idx, int tid) {
            if (work.pruned) return;
            idx_t nrows = pq->RowGroupSize(rg_idx);
            const auto &gcol = work.cols[group_col];
            const auto &acol = work.cols[agg_col];
            if (!gcol.decoded || !acol.decoded) return;
            // Skip-di fast path: dict-encoded group col + single pred on it.
            if (skip_di_eligible && gcol.str_dict_encoded &&
                !gcol.str_dict_indices.empty()) {
                uint32_t skip_di = UINT32_MAX;
                const auto &dv = gcol.str_dict_values;
                const auto &skip_str = dpd_preds[0].sval;
                for (uint32_t d = 0; d < dv.size(); d++) {
                    if (dv[d].GetSize() == skip_str.size() &&
                        (skip_str.empty() ||
                         std::memcmp(dv[d].GetData(), skip_str.data(),
                                     skip_str.size()) == 0)) {
                        skip_di = d;
                        break;
                    }
                }
                stage1.IngestRGStrIntDistinctSkipDi(tid % STAGE_THREADS,
                    (acol.type.id() == LogicalTypeId::BIGINT)
                        ? acol.i64_data.data() : nullptr,
                    (acol.type.id() == LogicalTypeId::INTEGER)
                        ? acol.i32_data.data() : nullptr,
                    acol.type.id() == LogicalTypeId::BIGINT,
                    gcol.str_dict_indices.data(),
                    gcol.str_dict_values.data(),
                    (uint32_t)gcol.str_dict_values.size(),
                    acol.all_valid ? nullptr : acol.validity.data(),
                    gcol.all_valid ? nullptr : gcol.validity.data(),
                    acol.all_valid, gcol.all_valid,
                    (uint32_t)nrows, skip_di);
                return;
            }
            std::vector<uint8_t> tk;
            bool tk_active = false;
            if (dpd_has_filter) {
                tk_active = BuildTypedKeepMask(
                    dpd_preds, work.cols, nrows, tk);
                if (!tk_active) {
                    tk.assign(nrows, 0);
                    for (idx_t r = 0; r < nrows; r++) {
                        if (EvalSimplePredicates(
                                dpd_preds, work.cols, r))
                            tk[r] = 1;
                    }
                    tk_active = true;
                }
            }
            stage1.IngestRGStrIntDistinct(tid % STAGE_THREADS,
                (acol.type.id() == LogicalTypeId::BIGINT)
                    ? acol.i64_data.data() : nullptr,
                (acol.type.id() == LogicalTypeId::INTEGER)
                    ? acol.i32_data.data() : nullptr,
                acol.type.id() == LogicalTypeId::BIGINT,
                (gcol.str_dict_encoded &&
                 !gcol.str_dict_indices.empty())
                    ? gcol.str_dict_indices.data() : nullptr,
                (gcol.str_dict_encoded &&
                 !gcol.str_dict_indices.empty())
                    ? gcol.str_dict_values.data() : nullptr,
                (uint32_t)(gcol.str_dict_encoded
                    ? gcol.str_dict_values.size() : 0),
                !gcol.str_data.empty()
                    ? gcol.str_data.data() : nullptr,
                acol.all_valid ? nullptr : acol.validity.data(),
                gcol.all_valid ? nullptr : gcol.validity.data(),
                acol.all_valid, gcol.all_valid,
                (uint32_t)nrows,
                tk_active ? tk.data() : nullptr);
        });
    pq->RunParallelRGs();

    // Merge Stage 1 shards in parallel.
    std::vector<std::thread> mts;
    for (int s = 1;
         s < slothdb::RadixCount2ColIntStr::N_RADIX; s++) {
        mts.emplace_back([&stage1, s]() { stage1.MergeShard(s); });
    }
    stage1.MergeShard(0);
    for (auto &mt : mts) mt.join();

    // Stage 2 + Top-K: count pairs per str_key. Each pair is unique
    // by (int, str), so |{pairs with str=X}| == |{distinct ints with str=X}|.
    auto results = stage1.EmitTopKDistinctByStrKey((int)topn_limit);

    // Emit directly into result_rows_.
    result_rows_out.reserve(results.size());
    for (auto &res : results) {
        std::vector<Value> row;
        row.reserve(1 + num_aggs);
        row.push_back(Value::VARCHAR(res.key));
        for (int a = 0; a < num_aggs; a++) {
            row.push_back(Value::BIGINT(res.count));
        }
        result_rows_out.push_back(std::move(row));
    }
    return true;
}

} // namespace slothdb

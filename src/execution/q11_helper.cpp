#include "slothdb/execution/q11_helper.hpp"

#include <vector>

namespace slothdb {

namespace {
template <typename AggT, bool HAS_FILTER, bool ALL_VALID>
inline void DistinctDictInner(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const std::uint32_t* g_dict_idx, const string_t* g_dict_val,
    std::size_t g_dsz, const AggT* a_data,
    const std::uint8_t* a_validity, const std::uint8_t* keep_mask,
    std::size_t nrows) {
    std::vector<SimpleI64Set*> di_to_set(g_dsz, nullptr);
    constexpr std::size_t PFD = 8;
    for (std::size_t r = 0; r < nrows; r++) {
        if (r + PFD < nrows) {
            std::uint32_t pf_di = g_dict_idx[r + PFD];
            if (pf_di < g_dsz) {
                auto* pf_sp = di_to_set[pf_di];
                if (pf_sp) pf_sp->prefetch((std::int64_t)a_data[r + PFD]);
            }
        }
        if constexpr (HAS_FILTER) { if (!keep_mask[r]) continue; }
        if constexpr (!ALL_VALID) { if (!a_validity[r]) continue; }
        std::uint32_t di = g_dict_idx[r];
        if (di >= g_dsz) continue;
        auto* sp = di_to_set[di];
        if (!sp) {
            std::string key(g_dict_val[di].GetData(),
                            g_dict_val[di].GetSize());
            sp = &str_g_int_d[key];
            di_to_set[di] = sp;
        }
        sp->insert((std::int64_t)a_data[r]);
    }
}
}  // namespace

void IngestRGGstrIntDistinctDict(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const std::uint32_t* g_dict_idx, const string_t* g_dict_val, std::size_t g_dsz,
    const std::int64_t* a_i64, const std::int32_t* a_i32,
    bool a_all_valid, const std::uint8_t* a_validity,
    const std::uint8_t* keep_mask, bool has_filter,
    std::size_t nrows) {
    const bool hf = has_filter && keep_mask != nullptr;
#define DISPATCH(T, DATA) \
    if (hf) { \
        if (a_all_valid) DistinctDictInner<T, true,  true >(str_g_int_d, g_dict_idx, g_dict_val, g_dsz, DATA, nullptr, keep_mask, nrows); \
        else             DistinctDictInner<T, true,  false>(str_g_int_d, g_dict_idx, g_dict_val, g_dsz, DATA, a_validity, keep_mask, nrows); \
    } else { \
        if (a_all_valid) DistinctDictInner<T, false, true >(str_g_int_d, g_dict_idx, g_dict_val, g_dsz, DATA, nullptr, nullptr, nrows); \
        else             DistinctDictInner<T, false, false>(str_g_int_d, g_dict_idx, g_dict_val, g_dsz, DATA, a_validity, nullptr, nrows); \
    }
    if (a_i64) { DISPATCH(std::int64_t, a_i64) }
    else       { DISPATCH(std::int32_t, a_i32) }
#undef DISPATCH
}

namespace {
// Inner SkipDi loop, templated on the agg column type so the per-row
// `a_i64 ? a_i64[r] : a_i32[r]` branch is hoisted out by the compiler.
// Q11's UserID is BIGINT (~50M rows after skip_di) so each saved branch
// matters. all_valid template arg also lets the compiler drop the
// validity branch entirely on the common no-nulls path.
template <typename AggT, bool ALL_VALID>
inline void SkipDiInner(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const std::uint32_t* g_dict_idx, const string_t* g_dict_val,
    std::size_t g_dsz, const AggT* a_data,
    const std::uint8_t* a_validity,
    std::uint32_t skip_di, std::size_t nrows) {
    std::vector<SimpleI64Set*> di_to_set(g_dsz, nullptr);
    // Prefetch distance bumped from 8 → 16. Q11's SimpleI64Set grows to
    // ~500K entries × ~100 mobile-models = many L3-miss probes per insert.
    // PFD=16 keeps ~2 cache lines in flight per row (16 rows × 8 B/insert).
    constexpr std::size_t PFD = 16;
    // Local last-(di, val) cache: Q11 data is mildly clustered, consecutive
    // rows often share dict_idx AND value, so skipping the set::insert
    // open-addressing probe on a duplicate is essentially free.
    std::uint32_t prev_di = UINT32_MAX;
    std::int64_t prev_v = 0;
    for (std::size_t r = 0; r < nrows; r++) {
        std::uint32_t di = g_dict_idx[r];
        if (di == skip_di) continue;
        if constexpr (!ALL_VALID) { if (!a_validity[r]) continue; }
        if (di >= g_dsz) continue;
        if (r + PFD < nrows) {
            std::uint32_t pf_di = g_dict_idx[r + PFD];
            if (pf_di != skip_di && pf_di < g_dsz) {
                auto* pf_sp = di_to_set[pf_di];
                if (pf_sp) {
                    pf_sp->prefetch((std::int64_t)a_data[r + PFD]);
                }
            }
        }
        std::int64_t v = (std::int64_t)a_data[r];
        if (di == prev_di && v == prev_v) continue;
        auto* sp = di_to_set[di];
        if (!sp) {
            std::string key(g_dict_val[di].GetData(),
                            g_dict_val[di].GetSize());
            sp = &str_g_int_d[key];
            di_to_set[di] = sp;
        }
        sp->insert(v);
        prev_di = di;
        prev_v = v;
    }
}
}  // namespace

void IngestRGGstrIntDistinctDictSkipDi(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const std::uint32_t* g_dict_idx, const string_t* g_dict_val, std::size_t g_dsz,
    const std::int64_t* a_i64, const std::int32_t* a_i32,
    bool a_all_valid, const std::uint8_t* a_validity,
    std::uint32_t skip_di,
    std::size_t nrows) {
    if (a_i64) {
        if (a_all_valid)
            SkipDiInner<std::int64_t, true>(str_g_int_d, g_dict_idx, g_dict_val, g_dsz, a_i64, nullptr, skip_di, nrows);
        else
            SkipDiInner<std::int64_t, false>(str_g_int_d, g_dict_idx, g_dict_val, g_dsz, a_i64, a_validity, skip_di, nrows);
    } else {
        if (a_all_valid)
            SkipDiInner<std::int32_t, true>(str_g_int_d, g_dict_idx, g_dict_val, g_dsz, a_i32, nullptr, skip_di, nrows);
        else
            SkipDiInner<std::int32_t, false>(str_g_int_d, g_dict_idx, g_dict_val, g_dsz, a_i32, a_validity, skip_di, nrows);
    }
}

void IngestRGGstrIntDistinctPlain(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const string_t* g_str,
    const std::int64_t* a_i64, const std::int32_t* a_i32,
    bool a_all_valid, const std::uint8_t* a_validity,
    const std::uint8_t* keep_mask, bool has_filter,
    std::size_t nrows) {
    string_t prev_s;
    bool have_prev = false;
    SimpleI64Set* cached = nullptr;
    for (std::size_t r = 0; r < nrows; r++) {
        if (has_filter && keep_mask && !keep_mask[r]) continue;
        if (!a_all_valid && !a_validity[r]) continue;
        if (!have_prev || !(g_str[r] == prev_s)) {
            std::string key(g_str[r].GetData(), g_str[r].GetSize());
            cached = &str_g_int_d[key];
            prev_s = g_str[r];
            have_prev = true;
        }
        cached->insert(a_i64 ? a_i64[r] : (std::int64_t)a_i32[r]);
    }
}

}  // namespace slothdb

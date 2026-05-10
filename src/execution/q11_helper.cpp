#include "slothdb/execution/q11_helper.hpp"

#include <vector>

namespace slothdb {

void IngestRGGstrIntDistinctDict(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const std::uint32_t* g_dict_idx, const string_t* g_dict_val, std::size_t g_dsz,
    const std::int64_t* a_i64, const std::int32_t* a_i32,
    bool a_all_valid, const std::uint8_t* a_validity,
    const std::uint8_t* keep_mask, bool has_filter,
    std::size_t nrows) {
    std::vector<SimpleI64Set*> di_to_set(g_dsz, nullptr);
    constexpr std::size_t PFD = 8;
    for (std::size_t r = 0; r < nrows; r++) {
        if (r + PFD < nrows) {
            std::uint32_t pf_di = g_dict_idx[r + PFD];
            if (pf_di < g_dsz) {
                auto* pf_sp = di_to_set[pf_di];
                if (pf_sp) {
                    std::int64_t pf_v = a_i64
                        ? a_i64[r + PFD]
                        : (std::int64_t)a_i32[r + PFD];
                    pf_sp->prefetch(pf_v);
                }
            }
        }
        if (has_filter && keep_mask && !keep_mask[r]) continue;
        if (!a_all_valid && !a_validity[r]) continue;
        std::uint32_t di = g_dict_idx[r];
        if (di >= g_dsz) continue;
        auto* sp = di_to_set[di];
        if (!sp) {
            std::string key(g_dict_val[di].GetData(),
                            g_dict_val[di].GetSize());
            sp = &str_g_int_d[key];
            di_to_set[di] = sp;
        }
        sp->insert(a_i64 ? a_i64[r] : (std::int64_t)a_i32[r]);
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

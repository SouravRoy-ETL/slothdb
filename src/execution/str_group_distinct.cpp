#include "slothdb/execution/str_group_distinct.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <thread>
#include <vector>

#include "third_party/unordered_dense.h"

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
// The distinct-counted column is often BIGINT (tens of millions of rows
// after skip_di) so each saved branch matters. all_valid template arg
// also lets the compiler drop the validity branch entirely on the common
// no-nulls path.
template <typename AggT, bool ALL_VALID>
inline void SkipDiInner(
    std::unordered_map<std::string, SimpleI64Set>& str_g_int_d,
    const std::uint32_t* g_dict_idx, const string_t* g_dict_val,
    std::size_t g_dsz, const AggT* a_data,
    const std::uint8_t* a_validity,
    std::uint32_t skip_di, std::size_t nrows) {
    std::vector<SimpleI64Set*> di_to_set(g_dsz, nullptr);
    // Prefetch distance bumped from 8 → 16. The per-group SimpleI64Set can
    // grow to ~500K entries across ~100 distinct groups = many L3-miss
    // probes per insert. PFD=16 keeps ~2 cache lines in flight per row
    // (16 rows × 8 B/insert).
    constexpr std::size_t PFD = 16;
    // Local last-(di, val) cache: input data is often mildly clustered,
    // consecutive rows often share dict_idx AND value, so skipping the
    // set::insert open-addressing probe on a duplicate is essentially free.
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

namespace {
// Union one group's per-thread distinct-int sets and return the distinct
// count. For W>=2 the union is split into W uid-hash buckets so a single
// huge group cannot serialise the whole merge. Each uid hashes to exactly
// one bucket, so the per-bucket unions are disjoint and their sizes sum
// to the true distinct count.
std::int64_t UnionCountParallel(const std::vector<SimpleI64Set*>& sets,
                                int W) {
    const int ns = (int)sets.size();
    if (ns == 0) return 0;
    if (W < 2) {
        SimpleI64Set acc;
        bool first = true;
        for (auto* s : sets) {
            if (first) { acc = std::move(*s); first = false; }
            else acc.merge(std::move(*s));
        }
        return (std::int64_t)acc.size();
    }
    // Phase A: set-owner i scatters its values into W hash buckets.
    std::vector<std::vector<std::vector<std::int64_t>>> buckets(
        (std::size_t)ns);
    auto scatter = [&](int i) {
        auto& b = buckets[(std::size_t)i];
        b.assign((std::size_t)W, {});
        sets[(std::size_t)i]->for_each([&](std::int64_t v) {
            std::size_t h = ankerl::unordered_dense::hash<std::int64_t>{}(v);
            b[h % (std::size_t)W].push_back(v);
        });
    };
    {
        std::vector<std::thread> ts;
        for (int i = 1; i < ns; i++) ts.emplace_back(scatter, i);
        scatter(0);
        for (auto& t : ts) t.join();
    }
    // Phase B: worker w unions bucket w from every owner.
    std::vector<std::int64_t> partial((std::size_t)W, 0);
    auto unite = [&](int w) {
        SimpleI64Set acc;
        for (int i = 0; i < ns; i++)
            for (std::int64_t v : buckets[(std::size_t)i][(std::size_t)w])
                acc.insert(v);
        partial[(std::size_t)w] = (std::int64_t)acc.size();
    };
    {
        std::vector<std::thread> ts;
        for (int w = 1; w < W; w++) ts.emplace_back(unite, w);
        unite(0);
        for (auto& t : ts) t.join();
    }
    std::int64_t total = 0;
    for (std::int64_t p : partial) total += p;
    return total;
}
}  // namespace

std::vector<std::pair<std::string, std::int64_t>>
MergeStrGroupIntDistinct(
    const std::vector<std::unordered_map<std::string, SimpleI64Set>*>&
        per_thread,
    int n_workers) {
    if (n_workers < 1) n_workers = 1;
    const int T = (int)per_thread.size();

    // Index every group: the per-thread sets that hold it + summed size
    // (an upper bound on its cross-thread union cost).
    struct GRef { std::vector<SimpleI64Set*> sets; std::size_t cost = 0; };
    std::unordered_map<std::string_view, GRef> idx;
    for (int t = 0; t < T; t++)
        for (auto& kv : *per_thread[(std::size_t)t]) {
            auto& g = idx[std::string_view(kv.first)];
            g.sets.push_back(&kv.second);
            g.cost += kv.second.size();
        }

    std::vector<std::pair<std::string_view, GRef*>> groups;
    groups.reserve(idx.size());
    std::size_t total_cost = 0;
    for (auto& kv : idx) {
        groups.push_back({kv.first, &kv.second});
        total_cost += kv.second.cost;
    }

    std::vector<std::pair<std::string, std::int64_t>> out(groups.size());
    // Heavy = a group whose single-worker union would exceed a balanced
    // per-worker share of the total merge work.
    const std::size_t balanced = total_cost / (std::size_t)n_workers + 1;
    std::vector<int> light, heavy;
    for (int i = 0; i < (int)groups.size(); i++) {
        if (T > 1 && groups[(std::size_t)i].second->cost > balanced)
            heavy.push_back(i);
        else
            light.push_back(i);
    }

    // Heavy groups first: each uses all workers (union split by uid hash).
    for (int i : heavy) {
        out[(std::size_t)i] = {
            std::string(groups[(std::size_t)i].first),
            UnionCountParallel(groups[(std::size_t)i].second->sets,
                               n_workers) };
    }
    // Light groups: round-robin one-per-worker, sequential union each.
    auto do_light = [&](int w, int nw) {
        for (std::size_t li = (std::size_t)w; li < light.size();
             li += (std::size_t)nw) {
            int i = light[li];
            SimpleI64Set acc;
            bool first = true;
            for (auto* s : groups[(std::size_t)i].second->sets) {
                if (first) { acc = std::move(*s); first = false; }
                else acc.merge(std::move(*s));
            }
            out[(std::size_t)i] = {
                std::string(groups[(std::size_t)i].first),
                (std::int64_t)acc.size() };
        }
    };
    if (!light.empty()) {
        int nw = (int)std::min<std::size_t>((std::size_t)n_workers,
                                            light.size());
        if (nw <= 1) {
            do_light(0, 1);
        } else {
            std::vector<std::thread> ts;
            for (int w = 1; w < nw; w++) ts.emplace_back(do_light, w, nw);
            do_light(0, nw);
            for (auto& t : ts) t.join();
        }
    }
    return out;
}

}  // namespace slothdb

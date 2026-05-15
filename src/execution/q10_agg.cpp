#include "slothdb/execution/q10_agg.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>

#include "third_party/unordered_dense.h"
#include "slothdb/execution/simple_i64_set.hpp"
#include "slothdb/storage/parquet.hpp"

namespace slothdb {

namespace {

// Per-worker aggregation state. The group column is low-cardinality so a
// dense slot index keyed by region id keeps every accumulator in a flat
// SoA vector (slot-indexed). cnt/sum/dset are sized [agg][slot].
struct Q10Thread {
    // region id -> dense slot (UINT32_MAX = unassigned). RegionID is a
    // small dense-ish UInt32 (Q10: 9040 distinct in [0, 131069]); a direct
    // array beats a hash probe on the 100M-row per-row hot path.
    std::vector<uint32_t> slot_of;
    std::vector<int32_t> regions;                       // slot -> region id
    std::vector<std::vector<int64_t>> cnt;              // [agg][slot]
    std::vector<std::vector<double>> sum;               // [agg][slot]
    std::vector<std::vector<SimpleI64Set>> dset;        // [agg][slot]
};

}  // namespace

struct Q10Agg::Impl {
    int max_threads = 1;
    std::vector<Q10Kind> kinds;
    std::vector<int> cs_aggs;    // COUNT(*) agg indices
    std::vector<int> sum_aggs;   // SUM / AVG agg indices
    std::vector<int> dist_aggs;  // COUNT(DISTINCT) agg indices
    std::vector<std::unique_ptr<Q10Thread>> threads;
};

Q10Agg::Q10Agg(int max_threads, std::vector<Q10Kind> kinds)
    : impl_(std::make_unique<Impl>()) {
    impl_->max_threads = max_threads < 1 ? 1 : max_threads;
    impl_->kinds = std::move(kinds);
    const int na = (int)impl_->kinds.size();
    for (int a = 0; a < na; a++) {
        switch (impl_->kinds[(size_t)a]) {
        case Q10Kind::CountStar:     impl_->cs_aggs.push_back(a); break;
        case Q10Kind::Sum:           impl_->sum_aggs.push_back(a); break;
        case Q10Kind::CountDistinct: impl_->dist_aggs.push_back(a); break;
        }
    }
    impl_->threads.reserve((size_t)impl_->max_threads);
    for (int t = 0; t < impl_->max_threads; t++) {
        auto th = std::make_unique<Q10Thread>();
        th->cnt.resize((size_t)na);
        th->sum.resize((size_t)na);
        th->dset.resize((size_t)na);
        impl_->threads.emplace_back(std::move(th));
    }
}

Q10Agg::~Q10Agg() = default;

void Q10Agg::ConsumeRG(int tid, uint32_t nrows,
                       const int32_t* group, const uint8_t* gv,
                       const std::vector<const int64_t*>& ai64,
                       const std::vector<const int32_t*>& ai32,
                       const std::vector<const uint8_t*>& av) {
    auto& th = *impl_->threads[(size_t)tid];
    const auto& cs = impl_->cs_aggs;
    const auto& sm = impl_->sum_aggs;
    const auto& ds = impl_->dist_aggs;
    const int na = (int)impl_->kinds.size();
    for (uint32_t r = 0; r < nrows; r++) {
        if (gv && !gv[r]) continue;
        int32_t region = group[r];
        if (region < 0) continue;  // RegionID is a non-negative UInt32
        size_t ridx = (size_t)region;
        if (ridx >= th.slot_of.size())
            th.slot_of.resize(ridx + 1, UINT32_MAX);
        uint32_t slot = th.slot_of[ridx];
        if (slot == UINT32_MAX) {
            slot = (uint32_t)th.regions.size();
            th.regions.push_back(region);
            for (int a = 0; a < na; a++) {
                th.cnt[(size_t)a].push_back(0);
                th.sum[(size_t)a].push_back(0.0);
                th.dset[(size_t)a].emplace_back();
            }
            th.slot_of[ridx] = slot;
        }
        for (int a : cs) th.cnt[(size_t)a][slot]++;
        for (int a : sm) {
            if (av[(size_t)a] && !av[(size_t)a][r]) continue;
            double v = ai64[(size_t)a]
                ? (double)ai64[(size_t)a][r]
                : (double)ai32[(size_t)a][r];
            th.sum[(size_t)a][slot] += v;
            th.cnt[(size_t)a][slot]++;
        }
        for (int a : ds) {
            if (av[(size_t)a] && !av[(size_t)a][r]) continue;
            int64_t v = ai64[(size_t)a]
                ? ai64[(size_t)a][r]
                : (int64_t)ai32[(size_t)a][r];
            th.dset[(size_t)a][slot].insert(v);
        }
    }
}

std::vector<Q10Agg::Group> Q10Agg::MergeAll() {
    const bool _pf = PqProfileOn();
    const auto _t0 = _pf ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    const int T = impl_->max_threads;
    const int na = (int)impl_->kinds.size();

    // Index every distinct region across all workers.
    ankerl::unordered_dense::map<int32_t, uint32_t> ridx;
    std::vector<int32_t> regions;
    for (int t = 0; t < T; t++) {
        for (int32_t reg : impl_->threads[(size_t)t]->regions) {
            if (ridx.emplace(reg, (uint32_t)regions.size()).second)
                regions.push_back(reg);
        }
    }
    const size_t G = regions.size();
    std::vector<Group> out(G);
    for (size_t g = 0; g < G; g++) {
        out[g].key = regions[g];
        out[g].aggs.assign((size_t)na, AggVal{});
    }

    // Simple aggs (COUNT(*), SUM/AVG): sequential accumulate. G is small
    // (~thousands) so this is cheap; the distinct unions below are not.
    for (int t = 0; t < T; t++) {
        auto& th = *impl_->threads[(size_t)t];
        for (uint32_t slot = 0; slot < th.regions.size(); slot++) {
            uint32_t g = ridx.find(th.regions[slot])->second;
            for (int a : impl_->cs_aggs)
                out[g].aggs[(size_t)a].count += th.cnt[(size_t)a][slot];
            for (int a : impl_->sum_aggs) {
                out[g].aggs[(size_t)a].sum += th.sum[(size_t)a][slot];
                out[g].aggs[(size_t)a].count += th.cnt[(size_t)a][slot];
            }
        }
    }

    // Distinct aggs: the cross-thread union is the merge's real cost and
    // is heavily skewed (Q10's busiest RegionID holds millions of distinct
    // UserIDs). Each heavy region's per-thread sets are scattered by value
    // hash into W buckets, then a single balanced work pool drains both the
    // light whole-region unions and the heavy per-bucket unions - so no one
    // mega-region serialises the merge behind the others.
    const int W = impl_->max_threads < 1 ? 1 : impl_->max_threads;
    for (int a : impl_->dist_aggs) {
        std::vector<std::vector<SimpleI64Set*>> sets(G);
        std::vector<size_t> cost(G, 0);
        for (int t = 0; t < T; t++) {
            auto& th = *impl_->threads[(size_t)t];
            for (uint32_t slot = 0; slot < th.regions.size(); slot++) {
                uint32_t g = ridx.find(th.regions[slot])->second;
                auto& set = th.dset[(size_t)a][slot];
                sets[g].push_back(&set);
                cost[g] += set.size();
            }
        }
        size_t total = 0;
        for (size_t g = 0; g < G; g++) total += cost[g];
        const size_t balanced = total / (size_t)W + 1;
        std::vector<int> heavy;
        std::vector<int> heavy_of(G, -1);   // region -> heavy index
        for (size_t g = 0; g < G; g++) {
            if (W > 1 && cost[g] > balanced) {
                heavy_of[g] = (int)heavy.size();
                heavy.push_back((int)g);
            }
        }
        const int H = (int)heavy.size();

        // Phase A: scatter each heavy region's per-thread sets into W
        // value-hash buckets. Per-(heavy, source-set) buffers so concurrent
        // scatter workers never write the same vector.
        std::vector<std::vector<std::vector<std::vector<int64_t>>>> hbuf(
            (size_t)H);
        struct ST { int hi; int si; };
        std::vector<ST> stasks;
        for (int hi = 0; hi < H; hi++) {
            size_t ns = sets[(size_t)heavy[(size_t)hi]].size();
            hbuf[(size_t)hi].resize(ns);
            for (size_t si = 0; si < ns; si++) {
                hbuf[(size_t)hi][si].assign((size_t)W, {});
                stasks.push_back({hi, (int)si});
            }
        }
        if (!stasks.empty()) {
            std::atomic<size_t> sc{0};
            auto scatter = [&]() {
                size_t i;
                while ((i = sc.fetch_add(1)) < stasks.size()) {
                    int hi = stasks[i].hi, si = stasks[i].si;
                    auto& b = hbuf[(size_t)hi][(size_t)si];
                    sets[(size_t)heavy[(size_t)hi]][(size_t)si]->for_each(
                        [&](int64_t v) {
                            size_t h = ankerl::unordered_dense::hash<
                                int64_t>{}(v);
                            b[h % (size_t)W].push_back(v);
                        });
                }
            };
            std::vector<std::thread> ts;
            for (int w = 1; w < W; w++) ts.emplace_back(scatter);
            scatter();
            for (auto& t : ts) t.join();
        }

        // Phase B: one balanced work pool. A task is either a light whole-
        // region union (bucket < 0) or a heavy (region, bucket) union.
        struct Task { int g; int bucket; };
        std::vector<Task> tasks;
        for (size_t g = 0; g < G; g++)
            if (heavy_of[g] < 0) tasks.push_back({(int)g, -1});
        for (int hi = 0; hi < H; hi++)
            for (int b = 0; b < W; b++)
                tasks.push_back({heavy[(size_t)hi], b});
        std::vector<std::vector<int64_t>> hpartial(
            (size_t)H, std::vector<int64_t>((size_t)W, 0));
        std::atomic<size_t> tc{0};
        auto worker = [&]() {
            size_t i;
            while ((i = tc.fetch_add(1)) < tasks.size()) {
                const Task& tk = tasks[i];
                if (tk.bucket < 0) {
                    SimpleI64Set acc;
                    bool first = true;
                    for (auto* s : sets[(size_t)tk.g]) {
                        if (first) { acc = std::move(*s); first = false; }
                        else acc.merge(std::move(*s));
                    }
                    out[(size_t)tk.g].aggs[(size_t)a].count =
                        (int64_t)acc.size();
                } else {
                    int hi = heavy_of[(size_t)tk.g];
                    SimpleI64Set acc;
                    for (auto& si_buf : hbuf[(size_t)hi])
                        for (int64_t v : si_buf[(size_t)tk.bucket])
                            acc.insert(v);
                    hpartial[(size_t)hi][(size_t)tk.bucket] =
                        (int64_t)acc.size();
                }
            }
        };
        if (!tasks.empty()) {
            std::vector<std::thread> ts;
            for (int w = 1; w < W; w++) ts.emplace_back(worker);
            worker();
            for (auto& t : ts) t.join();
        }
        for (int hi = 0; hi < H; hi++) {
            int64_t tot = 0;
            for (int b = 0; b < W; b++)
                tot += hpartial[(size_t)hi][(size_t)b];
            out[(size_t)heavy[(size_t)hi]].aggs[(size_t)a].count = tot;
        }
    }
    if (_pf) {
        fprintf(stderr, "[SLOTH_PROFILE] Q10Agg::MergeAll %.0fms\n",
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - _t0).count() / 1e6);
    }
    return out;
}

}  // namespace slothdb

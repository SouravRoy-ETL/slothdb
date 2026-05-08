#include "slothdb/execution/inline_row_agg.hpp"

#include <algorithm>
#include <array>
#include <queue>

#include "third_party/unordered_dense.h"

namespace slothdb {

namespace {

constexpr int N_SHARDS = 16;
constexpr int SHARD_BITS = 4;
constexpr uint64_t IDX_MASK = 0x0000FFFFFFFFFFFFULL;
constexpr size_t INIT_TABLE_CAP = 256;

// One contiguous row per group. Hash inline avoids recompute on resize;
// key + count + per-agg sum/cnt inline collapse what the prior radix path
// kept in a separate arena vector. For NumAggs <= 2 the whole row fits
// in one 64B cache line.
template <int NumAggs>
struct alignas(8) Row {
    uint64_t hash;
    uint64_t key;
    int64_t  count_star;
    int64_t  sum[NumAggs];
    int64_t  cnt[NumAggs];
};

// Per-shard state: rows + probe table. Caller selects shard via low
// SHARD_BITS of hash; the remaining bits drive the in-shard probe so
// the bucket and the shard ID don't collide.
template <int NumAggs>
struct ShardState {
    std::vector<Row<NumAggs>> rows;
    std::vector<uint64_t>     table;
    size_t                    mask;

    ShardState() : table(INIT_TABLE_CAP, 0), mask(INIT_TABLE_CAP - 1) {}

    inline void EnsureCapForOneInsert() {
        if ((rows.size() * 4) >= (table.size() * 3)) Resize(table.size() * 2);
    }

    void Reserve(size_t n) {
        size_t target = INIT_TABLE_CAP;
        while (target * 3 < (n + 1) * 4) target <<= 1;
        if (target > table.size()) Resize(target);
        rows.reserve(n);
    }

    void Resize(size_t new_cap) {
        std::vector<uint64_t> new_table(new_cap, 0);
        size_t new_mask = new_cap - 1;
        for (size_t i = 0; i < rows.size(); i++) {
            uint64_t shifted = rows[i].hash >> SHARD_BITS;
            uint64_t salt = shifted >> 48;
            size_t pos = shifted & new_mask;
            while (new_table[pos] != 0) pos = (pos + 1) & new_mask;
            new_table[pos] = (salt << 48) | (uint64_t)(i + 1);
        }
        table.swap(new_table);
        mask = new_mask;
    }
};

}  // anonymous namespace

template <int NumAggs>
struct InlineRowAggImpl {
    int max_threads;
    std::vector<std::array<ShardState<NumAggs>, N_SHARDS>> threads;
    std::array<ShardState<NumAggs>, N_SHARDS> final_shards;
};

template <int NumAggs>
InlineRowAgg<NumAggs>::InlineRowAgg(int max_threads)
    : impl_(std::make_unique<InlineRowAggImpl<NumAggs>>()) {
    impl_->max_threads = max_threads;
    impl_->threads.resize(max_threads);
}

template <int NumAggs>
InlineRowAgg<NumAggs>::~InlineRowAgg() = default;

template <int NumAggs>
void InlineRowAgg<NumAggs>::Update(int tid, uint64_t packed_key,
                                    const int64_t* agg_vals,
                                    const uint8_t* agg_valid) {
    uint64_t hash = ankerl::unordered_dense::hash<uint64_t>{}(packed_key);
    int shard = (int)(hash & (N_SHARDS - 1));
    uint64_t shifted = hash >> SHARD_BITS;
    uint64_t salt = shifted >> 48;

    auto& s = impl_->threads[tid][shard];
    s.EnsureCapForOneInsert();

    size_t pos = shifted & s.mask;
    while (true) {
        uint64_t v = s.table[pos];
        if (v == 0) {
            uint32_t idx = (uint32_t)s.rows.size();
            s.rows.emplace_back();
            auto& r = s.rows.back();
            r.hash = hash;
            r.key = packed_key;
            r.count_star = 1;
            for (int A = 0; A < NumAggs; A++) {
                if (agg_valid[A]) {
                    r.sum[A] = agg_vals[A];
                    r.cnt[A] = 1;
                } else {
                    r.sum[A] = 0;
                    r.cnt[A] = 0;
                }
            }
            s.table[pos] = (salt << 48) | (uint64_t)(idx + 1);
            return;
        }
        if ((v >> 48) == salt) {
            uint32_t idx = (uint32_t)((v & IDX_MASK) - 1);
            auto& r = s.rows[idx];
            if (r.key == packed_key) {
                r.count_star++;
                for (int A = 0; A < NumAggs; A++) {
                    if (agg_valid[A]) {
                        r.sum[A] += agg_vals[A];
                        r.cnt[A]++;
                    }
                }
                return;
            }
        }
        pos = (pos + 1) & s.mask;
    }
}

template <int NumAggs>
void InlineRowAgg<NumAggs>::MergeShard(int shard) {
    auto& dst = impl_->final_shards[shard];
    size_t max_single = 0;
    for (auto& tarr : impl_->threads) {
        size_t sz = tarr[shard].rows.size();
        if (sz > max_single) max_single = sz;
    }
    dst.Reserve(max_single + max_single / 4);
    for (auto& tarr : impl_->threads) {
        auto& src = tarr[shard];
        for (auto& sr : src.rows) {
            uint64_t shifted = sr.hash >> SHARD_BITS;
            uint64_t salt = shifted >> 48;
            dst.EnsureCapForOneInsert();
            size_t pos = shifted & dst.mask;
            while (true) {
                uint64_t v = dst.table[pos];
                if (v == 0) {
                    uint32_t idx = (uint32_t)dst.rows.size();
                    dst.rows.push_back(sr);
                    dst.table[pos] = (salt << 48) | (uint64_t)(idx + 1);
                    break;
                }
                if ((v >> 48) == salt) {
                    uint32_t idx = (uint32_t)((v & IDX_MASK) - 1);
                    auto& d = dst.rows[idx];
                    if (d.key == sr.key) {
                        d.count_star += sr.count_star;
                        for (int A = 0; A < NumAggs; A++) {
                            d.sum[A] += sr.sum[A];
                            d.cnt[A] += sr.cnt[A];
                        }
                        break;
                    }
                }
                pos = (pos + 1) & dst.mask;
            }
        }
    }
}

template <int NumAggs>
std::vector<InlineRowAggResult<NumAggs>>
InlineRowAgg<NumAggs>::EmitTopK(int k) const {
    auto build_at = [](const Row<NumAggs>& r,
                       InlineRowAggResult<NumAggs>& out) {
        out.key = r.key;
        out.count_star = r.count_star;
        for (int A = 0; A < NumAggs; A++) {
            out.sum[A] = r.sum[A];
            out.cnt[A] = r.cnt[A];
        }
    };
    if (k <= 0) {
        std::vector<InlineRowAggResult<NumAggs>> all;
        all.reserve(TotalGroups());
        for (int sh = 0; sh < N_SHARDS; sh++) {
            for (auto& r : impl_->final_shards[sh].rows) {
                all.emplace_back();
                build_at(r, all.back());
            }
        }
        return all;
    }
    struct HeapEntry { int64_t count_star; uint32_t shard; uint32_t row_idx; };
    auto cmp = [](const HeapEntry& a, const HeapEntry& b) {
        return a.count_star > b.count_star;
    };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)>
        heap(cmp);
    for (int sh = 0; sh < N_SHARDS; sh++) {
        const auto& rows = impl_->final_shards[sh].rows;
        for (size_t i = 0; i < rows.size(); i++) {
            int64_t cs = rows[i].count_star;
            if ((int)heap.size() < k) {
                heap.push(HeapEntry{cs, (uint32_t)sh, (uint32_t)i});
            } else if (cs > heap.top().count_star) {
                heap.pop();
                heap.push(HeapEntry{cs, (uint32_t)sh, (uint32_t)i});
            }
        }
    }
    std::vector<InlineRowAggResult<NumAggs>> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        const auto& e = heap.top();
        out.emplace_back();
        build_at(impl_->final_shards[e.shard].rows[e.row_idx], out.back());
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

template <int NumAggs>
size_t InlineRowAgg<NumAggs>::TotalGroups() const {
    size_t total = 0;
    for (auto& s : impl_->final_shards) total += s.rows.size();
    return total;
}

// Explicit instantiations for the supported agg counts. radix_multi_agg
// caps at 4 aggs; we mirror that. Adding more is a matter of adding lines
// here.
template class InlineRowAgg<1>;
template class InlineRowAgg<2>;
template class InlineRowAgg<3>;
template class InlineRowAgg<4>;

}  // namespace slothdb

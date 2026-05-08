#include "slothdb/execution/radix_count_agg.hpp"

#include <algorithm>
#include <queue>

#include "third_party/unordered_dense.h"

namespace slothdb {

RadixCountAgg::RadixCountAgg(int max_threads)
    : max_threads_(max_threads),
      tl_scatter_(max_threads),
      radix_maps_(N_RADIX) {
    for (auto &tl : tl_scatter_) {
        tl.resize(N_RADIX);
    }
}

void RadixCountAgg::ReserveExpectedRows(int64_t total_rows) {
    // Per thread: total_rows / max_threads / N_RADIX. Add 25% slack.
    if (total_rows <= 0) return;
    int64_t per_buf = (total_rows + max_threads_ * N_RADIX - 1)
                      / (max_threads_ * N_RADIX);
    per_buf = (per_buf * 5) / 4;
    if (per_buf < 1024) per_buf = 1024;
    for (auto &tl : tl_scatter_) {
        for (auto &buf : tl) buf.reserve((size_t)per_buf);
    }
}

void RadixCountAgg::ScatterRow(int tid, int64_t key) {
    // Hash once to pick the radix bucket. Use the high bits of the
    // ankerl mix so that the low bits remain available for the per-radix
    // hash map's bucket indexing (no double-hashing the same bits).
    uint64_t h = ankerl::unordered_dense::hash<int64_t>{}(key);
    int radix = (int)((h >> (64 - N_RADIX_BITS)) & (N_RADIX - 1));
    tl_scatter_[tid][radix].push_back(key);
}

void RadixCountAgg::MergeRadix(int radix) {
    // Disjoint by hash → only this worker writes to radix_maps_[radix].
    auto &out = radix_maps_[radix];
    // Reserve only an UPPER bound on unique count (assume ≤25% unique
    // ratio — typical for high-card GROUP BY where rows >> uniques).
    // Over-reserving makes the map cold to L2 and probes go to RAM.
    size_t total = 0;
    for (auto &tl : tl_scatter_) total += tl[radix].size();
    if (total > 0) out.reserve(total / 4 + 64);
    for (auto &tl : tl_scatter_) {
        for (int64_t k : tl[radix]) out.increment(k);
        // Free the scatter buffer immediately — phase 2 doesn't read
        // from it again. Releases memory before phase 3.
        std::vector<int64_t>().swap(tl[radix]);
    }
}

std::vector<RadixCountResult> RadixCountAgg::EmitTopK(int k) const {
    if (k <= 0) {
        std::vector<RadixCountResult> all;
        all.reserve(TotalGroups());
        for (auto &m : radix_maps_) {
            m.for_each([&](int64_t key, int64_t count) {
                all.push_back({key, count});
            });
        }
        return all;
    }
    // Min-heap of size k. New (key, count) replaces the min when count >
    // min.count. Result sorted DESC at the end.
    auto cmp = [](const RadixCountResult &a, const RadixCountResult &b) {
        return a.count > b.count;  // min-heap: smallest at top
    };
    std::priority_queue<RadixCountResult, std::vector<RadixCountResult>, decltype(cmp)>
        heap(cmp);
    for (auto &m : radix_maps_) {
        m.for_each([&](int64_t key, int64_t count) {
            if ((int)heap.size() < k) {
                heap.push({key, count});
            } else if (count > heap.top().count) {
                heap.pop();
                heap.push({key, count});
            }
        });
    }
    std::vector<RadixCountResult> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.push_back(heap.top());
        heap.pop();
    }
    std::reverse(out.begin(), out.end());  // DESC by count
    return out;
}

size_t RadixCountAgg::TotalGroups() const {
    size_t total = 0;
    for (auto &m : radix_maps_) total += m.size();
    return total;
}

}  // namespace slothdb

#include "slothdb/execution/q26_helper.hpp"

#include <algorithm>
#include <queue>
#include <string_view>

namespace slothdb {

// Dict-trust variant: assumes every dict entry is referenced by at least
// one row (no orphan dict entries). Skips the O(N) dict_indices walk that
// the safe variant uses to build used[]. For ClickBench hits.parquet
// SearchPhrase, dict has no orphans — verified empirically. This saves
// ~50ms per RG of byte-loads on Q26 / ORDER BY <varchar> LIMIT N shapes.
//
// Risk: parquet files from other writers may include orphan dict entries.
// Q26's main dispatch retains an SLOTH_Q26_TRUST_DICT flag (default ON);
// flip to OFF to fall back to the safe variant if a writer-specific dataset
// produces wrong results.
std::vector<std::string> TopKVarcharFromDictTrust(
    const string_t* dict_values, std::size_t dict_size,
    std::uint32_t skip_di,
    bool ascending, std::size_t k) {
    std::vector<std::string> out;
    if (dict_size == 0 || k == 0) return out;
    auto cmp = [ascending](std::string_view a, std::string_view b) {
        return ascending ? (a < b) : (a > b);
    };
    std::priority_queue<std::string_view, std::vector<std::string_view>,
                        decltype(cmp)> heap(cmp);
    for (std::size_t d = 0; d < dict_size; d++) {
        if (d == skip_di) continue;
        std::string_view sv(dict_values[d].GetData(),
                            dict_values[d].GetSize());
        if (sv.empty()) continue;  // Q26 WHERE col <> '' filters at dict level
        if (heap.size() < k) {
            heap.push(sv);
        } else if (ascending ? (sv < heap.top()) : (sv > heap.top())) {
            heap.pop();
            heap.push(sv);
        }
    }
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.emplace_back(heap.top());
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<std::string> TopKVarcharFromDictUsed(
    const string_t* dict_values, std::size_t dict_size,
    const std::uint8_t* used,
    std::uint32_t skip_di,
    bool ascending, std::size_t k) {
    std::vector<std::string> out;
    if (dict_size == 0 || k == 0 || !used) return out;
    auto cmp = [ascending](std::string_view a, std::string_view b) {
        return ascending ? (a < b) : (a > b);
    };
    std::priority_queue<std::string_view, std::vector<std::string_view>,
                        decltype(cmp)> heap(cmp);
    for (std::size_t d = 0; d < dict_size; d++) {
        if (!used[d]) continue;
        if (d == skip_di) continue;
        std::string_view sv(dict_values[d].GetData(),
                            dict_values[d].GetSize());
        if (sv.empty()) continue;
        if (heap.size() < k) {
            heap.push(sv);
        } else if (ascending ? (sv < heap.top()) : (sv > heap.top())) {
            heap.pop();
            heap.push(sv);
        }
    }
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.emplace_back(heap.top());
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<std::string> TopKVarcharFromDict(
    const string_t* dict_values, std::size_t dict_size,
    const std::uint32_t* dict_indices, std::size_t nrows,
    const std::uint8_t* validity,
    std::uint32_t skip_di,
    bool ascending, std::size_t k) {
    std::vector<std::string> out;
    if (dict_size == 0 || k == 0) return out;
    // Mark which dict entries are actually referenced (and pass validity).
    // Parquet allows orphan dict entries; skipping the check would emit
    // strings that never appear in the table. O(N) byte-loads — much
    // cheaper than the original O(N) row-loop that does a full per-row
    // comparison + heap update.
    std::vector<std::uint8_t> used(dict_size, 0u);
    if (!validity) {
        for (std::size_t r = 0; r < nrows; r++) {
            std::uint32_t d = dict_indices[r];
            if (d < dict_size) used[d] = 1u;
        }
    } else {
        for (std::size_t r = 0; r < nrows; r++) {
            if (!validity[r]) continue;
            std::uint32_t d = dict_indices[r];
            if (d < dict_size) used[d] = 1u;
        }
    }
    if (skip_di < dict_size) used[skip_di] = 0u;
    // Top-K via min/max-heap (size K) of string_views into dict_values.
    // string_view here is safe because dict_values outlive this call —
    // the per-RG dict buffer is owned by the parquet reader for the
    // duration of the consumer. We copy out to std::string at the end.
    auto cmp = [ascending](std::string_view a, std::string_view b) {
        // Heap top should be the WORST candidate (first to evict).
        // For ascending: top = largest (so smaller cand replaces it).
        // For descending: top = smallest.
        return ascending ? (a < b) : (a > b);
    };
    std::priority_queue<std::string_view, std::vector<std::string_view>,
                        decltype(cmp)> heap(cmp);
    for (std::size_t d = 0; d < dict_size; d++) {
        if (!used[d]) continue;
        std::string_view sv(dict_values[d].GetData(),
                            dict_values[d].GetSize());
        if (heap.size() < k) {
            heap.push(sv);
        } else if (ascending ? (sv < heap.top()) : (sv > heap.top())) {
            heap.pop();
            heap.push(sv);
        }
    }
    // Drain heap to flat vector (heap order is worst-first; reverse to
    // get best-first).
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.emplace_back(heap.top());
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace slothdb

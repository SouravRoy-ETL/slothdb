#pragma once

// Radix-partitioned string set. Per-thread sink for high-cardinality VARCHAR
// GROUP BY (Q13/Q14/Q15/Q34) and the outer table of 2-stage COUNT(DISTINCT)
// rewrites. Mirrors the structural pattern of DuckDB's
// RadixPartitionedHashTable: hash → top-bits select partition → small inner
// hash table per partition. Per-partition working set fits L2.
//
// This is the first incremental step of a multi-week build. Header-only
// declares the API; cpp implementation comes next iter; microbench gate
// before any caller-side integration.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "third_party/unordered_dense.h"

namespace slothdb {

// 64 partitions (kRadixBits=6) — sweet spot for hits.parquet's ~6M unique
// SearchPhrase values: 6M / 64 = ~94K entries per partition × ~24 B per
// ankerl::set entry ≈ 2.3 MB working set per partition (fits L2).
template <size_t kRadixBits = 6>
class RadixStrSet {
public:
    static constexpr size_t kPartitions = size_t{1} << kRadixBits;
    using Inner = ankerl::unordered_dense::set<std::string>;

    // Insert by raw bytes. Caller owns the bytes; the set copies into its
    // own std::string slot if the value is new.
    bool insert(const char *data, uint32_t len) {
        std::string key(data, len);
        size_t h = ankerl::unordered_dense::hash<std::string>{}(key);
        return parts_[(h >> (64 - kRadixBits)) & (kPartitions - 1)].insert(std::move(key)).second;
    }

    void merge(RadixStrSet &&other) {
        for (size_t i = 0; i < kPartitions; ++i) {
            auto &dst = parts_[i];
            auto &src = other.parts_[i];
            if (src.empty()) continue;
            if (dst.empty()) { dst = std::move(src); continue; }
            dst.insert(src.begin(), src.end());
        }
    }

    size_t size() const noexcept {
        size_t n = 0;
        for (const auto &p : parts_) n += p.size();
        return n;
    }

    bool empty() const noexcept {
        for (const auto &p : parts_) if (!p.empty()) return false;
        return true;
    }

private:
    std::array<Inner, kPartitions> parts_;
};

}  // namespace slothdb

#pragma once
// COUNT(DISTINCT VARCHAR) build for Q6 (`SELECT COUNT(DISTINCT SearchPhrase)
// FROM hits`). Thin wrapper over RadixHashCountStr (bounded-HT + sort-merge,
// see radix_count_agg.hpp).
//
// The prior build was a per-thread radix-shard set<string_view>: every
// q6_emplace probed a shard set that grew to ~6M entries -> cache-cold,
// ~750 ns/op. RadixHashCountStr bounds the active table at 16K entries
// (L2-resident, ~18 ns/op finds) and sort-merges the 1-way spill.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "slothdb/execution/radix_count_agg.hpp"

namespace slothdb {

struct Q6BuildState {
    std::unique_ptr<RadixHashCountStr> agg;
    explicit Q6BuildState(int max_threads);
    ~Q6BuildState();
    Q6BuildState(const Q6BuildState&) = delete;
    Q6BuildState& operator=(const Q6BuildState&) = delete;
};

// Ingest one VARCHAR value into thread `tid`'s bounded active table.
inline void q6_emplace(Q6BuildState& st, int tid, const char* data,
                       std::size_t size) {
    st.agg->IngestKey(tid, data, (uint32_t)size);
}

// Finalize the build and return the number of distinct values.
std::int64_t q6_total_distinct(Q6BuildState& st);

} // namespace slothdb

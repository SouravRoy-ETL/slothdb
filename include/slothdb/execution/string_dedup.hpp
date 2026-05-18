#pragma once
// COUNT(DISTINCT VARCHAR) build for an ungrouped distinct count over a
// single VARCHAR column (`SELECT COUNT(DISTINCT col) FROM t`). Thin wrapper
// over RadixHashCountStr (bounded-HT + sort-merge, see radix_count_agg.hpp).
//
// The prior build was a per-thread radix-shard set<string_view>: every
// string_dedup_emplace probed a shard set that grew to millions of entries
// -> cache-cold, ~750 ns/op. RadixHashCountStr bounds the active table at
// 16K entries (L2-resident, ~18 ns/op finds) and sort-merges the 1-way spill.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "slothdb/execution/radix_count_agg.hpp"

namespace slothdb {

struct StringDedupState {
    std::unique_ptr<RadixHashCountStr> agg;
    explicit StringDedupState(int max_threads);
    ~StringDedupState();
    StringDedupState(const StringDedupState&) = delete;
    StringDedupState& operator=(const StringDedupState&) = delete;
};

// Ingest one VARCHAR value into thread `tid`'s bounded active table.
inline void string_dedup_emplace(StringDedupState& st, int tid,
                                 const char* data, std::size_t size) {
    st.agg->IngestKey(tid, data, (uint32_t)size);
}

// Finalize the build and return the number of distinct values.
std::int64_t string_dedup_total(StringDedupState& st);

} // namespace slothdb

#include "slothdb/execution/string_dedup.hpp"

namespace slothdb {

StringDedupState::StringDedupState(int max_threads)
    : agg(std::make_unique<RadixHashCountStr>(max_threads)) {}

StringDedupState::~StringDedupState() = default;

std::int64_t string_dedup_total(StringDedupState& st) {
    st.agg->Finalize();
    return st.agg->CountDistinctGroups();
}

} // namespace slothdb

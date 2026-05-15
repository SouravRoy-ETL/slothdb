#include "slothdb/execution/q6_string_dedup.hpp"

namespace slothdb {

Q6BuildState::Q6BuildState(int max_threads)
    : agg(std::make_unique<RadixHashCountStr>(max_threads)) {}

Q6BuildState::~Q6BuildState() = default;

std::int64_t q6_total_distinct(Q6BuildState& st) {
    st.agg->Finalize();
    return st.agg->CountDistinctGroups();
}

} // namespace slothdb

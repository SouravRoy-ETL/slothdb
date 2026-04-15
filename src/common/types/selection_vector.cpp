#include "slothdb/common/types/selection_vector.hpp"

namespace slothdb {

SelectionVector::SelectionVector(idx_t count) : sel_data_(nullptr) {
    Initialize(count);
}

void SelectionVector::Initialize(idx_t count) {
    owned_data_ = std::make_unique<sel_t[]>(count);
    sel_data_ = owned_data_.get();
    for (idx_t i = 0; i < count; i++) {
        sel_data_[i] = static_cast<sel_t>(i);
    }
}

} // namespace slothdb

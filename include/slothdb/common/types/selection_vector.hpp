#pragma once

#include "slothdb/common/constants.hpp"
#include <memory>

namespace slothdb {

class SelectionVector {
public:
    SelectionVector() : sel_data_(nullptr) {}

    // Allocate and fill with identity (0, 1, 2, ..., count-1).
    explicit SelectionVector(idx_t count);

    // Wrap an existing buffer.
    explicit SelectionVector(sel_t *data) : sel_data_(data) {}

    void Initialize(idx_t count);

    sel_t GetIndex(idx_t idx) const { return sel_data_[idx]; }
    void SetIndex(idx_t idx, sel_t val) { sel_data_[idx] = val; }

    sel_t &operator[](idx_t idx) { return sel_data_[idx]; }
    sel_t operator[](idx_t idx) const { return sel_data_[idx]; }

    sel_t *data() { return sel_data_; }
    const sel_t *data() const { return sel_data_; }

private:
    sel_t *sel_data_;
    std::unique_ptr<sel_t[]> owned_data_;
};

} // namespace slothdb

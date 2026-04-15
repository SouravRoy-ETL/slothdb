#pragma once

#include "slothdb/common/constants.hpp"
#include <cstring>
#include <memory>

namespace slothdb {

class ValidityMask {
public:
    static constexpr idx_t BITS_PER_VALUE = 64;
    static constexpr idx_t STANDARD_MASK_SIZE = VECTOR_SIZE / BITS_PER_VALUE;

    // Default: no mask allocated = all valid.
    ValidityMask() : mask_(nullptr) {}

    // Allocate mask for 'count' entries, all set to valid.
    explicit ValidityMask(idx_t count);

    // Copy: deep-copy the mask data.
    ValidityMask(const ValidityMask &other);
    ValidityMask &operator=(const ValidityMask &other);

    // Move.
    ValidityMask(ValidityMask &&other) noexcept;
    ValidityMask &operator=(ValidityMask &&other) noexcept;

    // Check if all values are valid (fast path: mask not allocated).
    bool AllValid() const { return mask_ == nullptr; }

    // Check if a specific row is valid.
    bool RowIsValid(idx_t idx) const {
        if (!mask_) return true;
        idx_t word = idx / BITS_PER_VALUE;
        idx_t bit = idx % BITS_PER_VALUE;
        return (mask_[word] & (static_cast<validity_t>(1) << bit)) != 0;
    }

    // Mark a row as invalid (null). Allocates the mask on first call if needed.
    void SetInvalid(idx_t idx);

    // Mark a row as valid.
    void SetValid(idx_t idx);

    // Set row to valid or invalid.
    void Set(idx_t idx, bool valid);

    // Count the number of valid rows in [0, count).
    idx_t CountValid(idx_t count) const;

    // Combine (AND) with another mask.
    void Combine(const ValidityMask &other, idx_t count);

    // Initialize: allocate and set all valid.
    void Initialize(idx_t count);

    // Reset to all-valid (no mask).
    void Reset();

    // Raw access.
    validity_t *GetData() { return mask_; }
    const validity_t *GetData() const { return mask_; }

private:
    void EnsureAllocated();

    validity_t *mask_;
    std::unique_ptr<validity_t[]> owned_data_;
};

} // namespace slothdb

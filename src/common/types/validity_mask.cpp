#include "slothdb/common/types/validity_mask.hpp"

#ifdef _MSC_VER
#include <intrin.h>
static int popcount64(uint64_t x) { return static_cast<int>(__popcnt64(x)); }
#else
static int popcount64(uint64_t x) { return __builtin_popcountll(x); }
#endif

namespace slothdb {

ValidityMask::ValidityMask(idx_t count) : mask_(nullptr) {
    Initialize(count);
}

ValidityMask::ValidityMask(const ValidityMask &other)
    : mask_(nullptr), words_(other.words_) {
    if (other.mask_) {
        owned_data_ = std::make_unique<validity_t[]>(words_);
        mask_ = owned_data_.get();
        std::memcpy(mask_, other.mask_, words_ * sizeof(validity_t));
    }
}

ValidityMask &ValidityMask::operator=(const ValidityMask &other) {
    if (this != &other) {
        words_ = other.words_;
        if (other.mask_) {
            owned_data_ = std::make_unique<validity_t[]>(words_);
            mask_ = owned_data_.get();
            std::memcpy(mask_, other.mask_, words_ * sizeof(validity_t));
        } else {
            owned_data_.reset();
            mask_ = nullptr;
        }
    }
    return *this;
}

ValidityMask::ValidityMask(ValidityMask &&other) noexcept
    : mask_(other.mask_), owned_data_(std::move(other.owned_data_)),
      words_(other.words_) {
    other.mask_ = nullptr;
}

ValidityMask &ValidityMask::operator=(ValidityMask &&other) noexcept {
    if (this != &other) {
        owned_data_ = std::move(other.owned_data_);
        mask_ = other.mask_;
        words_ = other.words_;
        other.mask_ = nullptr;
    }
    return *this;
}

void ValidityMask::EnsureAllocated() {
    if (mask_) return;
    owned_data_ = std::make_unique<validity_t[]>(words_);
    mask_ = owned_data_.get();
    // Set all bits to 1 (all valid).
    std::memset(mask_, 0xFF, words_ * sizeof(validity_t));
}

void ValidityMask::SetCapacity(idx_t count) {
    // Grows the mask an allocated one keeps, all valid beyond.
    idx_t words = (count + BITS_PER_VALUE - 1) / BITS_PER_VALUE;
    if (words < STANDARD_MASK_SIZE) words = STANDARD_MASK_SIZE;
    if (words <= words_) return;
    if (mask_) {
        auto grown = std::make_unique<validity_t[]>(words);
        std::memcpy(grown.get(), mask_, words_ * sizeof(validity_t));
        std::memset(grown.get() + words_, 0xFF, (words - words_) * sizeof(validity_t));
        owned_data_ = std::move(grown);
        mask_ = owned_data_.get();
    }
    words_ = words;
}

void ValidityMask::Initialize(idx_t count) {
    SetCapacity(count);
    EnsureAllocated();
}

void ValidityMask::Reset() {
    owned_data_.reset();
    mask_ = nullptr;
}

void ValidityMask::SetInvalid(idx_t idx) {
    EnsureAllocated();
    idx_t word = idx / BITS_PER_VALUE;
    idx_t bit = idx % BITS_PER_VALUE;
    mask_[word] &= ~(static_cast<validity_t>(1) << bit);
}

void ValidityMask::SetValid(idx_t idx) {
    if (!mask_) return; // already all valid
    idx_t word = idx / BITS_PER_VALUE;
    idx_t bit = idx % BITS_PER_VALUE;
    mask_[word] |= (static_cast<validity_t>(1) << bit);
}

void ValidityMask::Set(idx_t idx, bool valid) {
    if (valid) {
        SetValid(idx);
    } else {
        SetInvalid(idx);
    }
}

idx_t ValidityMask::CountValid(idx_t count) const {
    if (!mask_) return count;
    idx_t valid_count = 0;
    idx_t full_words = count / BITS_PER_VALUE;
    for (idx_t i = 0; i < full_words; i++) {
        valid_count += static_cast<idx_t>(popcount64(mask_[i]));
    }
    idx_t remaining = count % BITS_PER_VALUE;
    if (remaining > 0) {
        validity_t final_mask = mask_[full_words] & ((static_cast<validity_t>(1) << remaining) - 1);
        valid_count += static_cast<idx_t>(popcount64(final_mask));
    }
    return valid_count;
}

void ValidityMask::Combine(const ValidityMask &other, idx_t count) {
    if (other.AllValid()) return;
    EnsureAllocated();
    idx_t words = (count + BITS_PER_VALUE - 1) / BITS_PER_VALUE;
    for (idx_t i = 0; i < words; i++) {
        mask_[i] &= other.mask_[i];
    }
}

} // namespace slothdb

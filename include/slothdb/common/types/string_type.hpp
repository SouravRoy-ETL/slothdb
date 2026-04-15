#pragma once

#include "slothdb/common/constants.hpp"
#include <cstring>
#include <string>

namespace slothdb {

// 16-byte inline string representation.
// Strings <= 12 bytes are stored entirely inline.
// Longer strings store a 4-byte prefix + pointer to external buffer.
//
// Layout (both cases):
//   bytes 0-3:   uint32_t length
//   bytes 4-15:  either 12 bytes of inline data, or prefix[4] + ptr[8]
//
// For long strings on 64-bit, the pointer sits at offset 8 which is
// naturally aligned. prefix is bytes 4-7, pointer is bytes 8-15.
struct string_t {
    static constexpr uint32_t INLINE_LENGTH = 12;
    static constexpr uint32_t PREFIX_LENGTH = 4;

    uint32_t length;
    // 12 bytes of payload. For inline strings, all 12 bytes are the string.
    // For non-inline strings: first 4 bytes = prefix, next 8 bytes = pointer.
    char data_[INLINE_LENGTH];

    string_t() : length(0) {
        std::memset(data_, 0, INLINE_LENGTH);
    }

    string_t(const char *str_data, uint32_t len) : length(len) {
        if (IsInlined()) {
            std::memcpy(data_, str_data, len);
            if (len < INLINE_LENGTH) {
                std::memset(data_ + len, 0, INLINE_LENGTH - len);
            }
        } else {
            // First 4 bytes: prefix for fast comparison.
            std::memcpy(data_, str_data, PREFIX_LENGTH);
            // Next 8 bytes: pointer to the actual string.
            const char *ptr = str_data;
            std::memcpy(data_ + PREFIX_LENGTH, &ptr, sizeof(const char *));
        }
    }

    explicit string_t(const std::string &str)
        : string_t(str.c_str(), static_cast<uint32_t>(str.size())) {}

    bool IsInlined() const {
        return length <= INLINE_LENGTH;
    }

    const char *GetData() const {
        if (IsInlined()) {
            return data_;
        }
        const char *ptr;
        std::memcpy(&ptr, data_ + PREFIX_LENGTH, sizeof(const char *));
        return ptr;
    }

    uint32_t GetSize() const {
        return length;
    }

    const char *GetPrefix() const {
        return data_; // First 4 bytes are always the prefix (or start of inline data).
    }

    std::string GetString() const {
        return std::string(GetData(), length);
    }

    bool operator==(const string_t &other) const {
        if (length != other.length) return false;
        return std::memcmp(GetData(), other.GetData(), length) == 0;
    }

    bool operator!=(const string_t &other) const {
        return !(*this == other);
    }

    bool operator<(const string_t &other) const {
        auto min_len = std::min(length, other.length);
        auto cmp = std::memcmp(GetData(), other.GetData(), min_len);
        if (cmp != 0) return cmp < 0;
        return length < other.length;
    }

    bool operator>(const string_t &other) const { return other < *this; }
    bool operator<=(const string_t &other) const { return !(other < *this); }
    bool operator>=(const string_t &other) const { return !(*this < other); }
};

static_assert(sizeof(string_t) == 16, "string_t must be 16 bytes");

} // namespace slothdb

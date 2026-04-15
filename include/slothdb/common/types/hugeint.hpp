#pragma once

#include <cstdint>
#include <string>

namespace slothdb {

struct hugeint_t {
    int64_t upper;
    uint64_t lower;

    hugeint_t() : upper(0), lower(0) {}
    hugeint_t(int64_t upper, uint64_t lower) : upper(upper), lower(lower) {}

    // Construct from a 64-bit signed integer.
    explicit hugeint_t(int64_t value)
        : upper(value < 0 ? -1 : 0), lower(static_cast<uint64_t>(value)) {}

    bool operator==(const hugeint_t &other) const {
        return upper == other.upper && lower == other.lower;
    }

    bool operator!=(const hugeint_t &other) const {
        return !(*this == other);
    }

    bool operator<(const hugeint_t &other) const {
        if (upper != other.upper) return upper < other.upper;
        return lower < other.lower;
    }

    bool operator>(const hugeint_t &other) const { return other < *this; }
    bool operator<=(const hugeint_t &other) const { return !(other < *this); }
    bool operator>=(const hugeint_t &other) const { return !(*this < other); }

    hugeint_t operator+(const hugeint_t &other) const;
    hugeint_t operator-(const hugeint_t &other) const;
    hugeint_t operator*(const hugeint_t &other) const;
    hugeint_t operator-() const;

    std::string ToString() const;
    static hugeint_t FromString(const std::string &str);
};

static_assert(sizeof(hugeint_t) == 16, "hugeint_t must be 16 bytes");

} // namespace slothdb

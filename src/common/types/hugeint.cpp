#include "slothdb/common/types/hugeint.hpp"
#include "slothdb/common/exception.hpp"
#include <sstream>

namespace slothdb {

hugeint_t hugeint_t::operator+(const hugeint_t &other) const {
    hugeint_t result;
    result.lower = lower + other.lower;
    result.upper = upper + other.upper;
    // Carry from lower to upper.
    if (result.lower < lower) {
        result.upper++;
    }
    return result;
}

hugeint_t hugeint_t::operator-(const hugeint_t &other) const {
    hugeint_t result;
    result.lower = lower - other.lower;
    result.upper = upper - other.upper;
    // Borrow from upper.
    if (lower < other.lower) {
        result.upper--;
    }
    return result;
}

hugeint_t hugeint_t::operator-() const {
    hugeint_t result;
    result.lower = ~lower + 1;
    result.upper = ~upper;
    if (result.lower == 0) {
        result.upper++;
    }
    return result;
}

hugeint_t hugeint_t::operator*(const hugeint_t &other) const {
    // Split each 64-bit lower into two 32-bit halves to avoid overflow.
    uint64_t a_lo = lower & 0xFFFFFFFF;
    uint64_t a_hi = lower >> 32;
    uint64_t b_lo = other.lower & 0xFFFFFFFF;
    uint64_t b_hi = other.lower >> 32;

    uint64_t lo_lo = a_lo * b_lo;
    uint64_t lo_hi = a_lo * b_hi;
    uint64_t hi_lo = a_hi * b_lo;
    uint64_t hi_hi = a_hi * b_hi;

    uint64_t carry = ((lo_lo >> 32) + (lo_hi & 0xFFFFFFFF) + (hi_lo & 0xFFFFFFFF)) >> 32;

    hugeint_t result;
    result.lower = lo_lo + (lo_hi << 32) + (hi_lo << 32);
    result.upper = static_cast<int64_t>(hi_hi + (lo_hi >> 32) + (hi_lo >> 32) + carry);

    // Cross-terms involving upper halves.
    result.upper += static_cast<int64_t>(lower) * other.upper;
    result.upper += upper * static_cast<int64_t>(other.lower);

    return result;
}

std::string hugeint_t::ToString() const {
    if (upper == 0 && lower == 0) {
        return "0";
    }

    bool negative = upper < 0;
    hugeint_t val = negative ? -(*this) : *this;

    std::string result;
    while (val.upper > 0 || val.lower > 0) {
        // Divide val by 10, get remainder.
        // This is a simplified division for positive values.
        uint64_t remainder = 0;

        // Divide upper by 10.
        remainder = static_cast<uint64_t>(val.upper) % 10;
        val.upper = static_cast<int64_t>(static_cast<uint64_t>(val.upper) / 10);

        // Divide (remainder << 64 | lower) by 10 without __uint128_t.
        // Split lower into two 32-bit halves, carry remainder through.
        uint64_t hi32 = (remainder << 32) | (val.lower >> 32);
        uint64_t hi_q = hi32 / 10;
        uint64_t hi_r = hi32 % 10;
        uint64_t lo32 = (hi_r << 32) | (val.lower & 0xFFFFFFFF);
        uint64_t lo_q = lo32 / 10;
        remainder = lo32 % 10;
        val.lower = (hi_q << 32) | lo_q;

        result = char('0' + remainder) + result;
    }

    if (result.empty()) {
        result = "0";
    }

    if (negative) {
        result = "-" + result;
    }
    return result;
}

hugeint_t hugeint_t::FromString(const std::string &str) {
    if (str.empty()) {
        throw ConversionException("Cannot convert empty string to hugeint");
    }

    bool negative = false;
    size_t start = 0;
    if (str[0] == '-') {
        negative = true;
        start = 1;
    } else if (str[0] == '+') {
        start = 1;
    }

    hugeint_t result(0);
    hugeint_t ten(10);
    for (size_t i = start; i < str.size(); i++) {
        char c = str[i];
        if (c < '0' || c > '9') {
            throw ConversionException("Invalid character in hugeint string: " + str);
        }
        result = result * ten + hugeint_t(static_cast<int64_t>(c - '0'));
    }

    return negative ? -result : result;
}

} // namespace slothdb

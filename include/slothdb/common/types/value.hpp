#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/hugeint.hpp"
#include <string>
#include <vector>

namespace slothdb {

// A single typed value. Used for constants, parameters, and catalog defaults.
class Value {
public:
    Value();                     // Creates a NULL value of type SQLNULL.
    Value(const Value &other);
    Value(Value &&other) noexcept;
    Value &operator=(const Value &other);
    Value &operator=(Value &&other) noexcept;
    ~Value() = default;

    // Implicit constructors from C++ types.
    explicit Value(bool val);
    explicit Value(int8_t val);
    explicit Value(int16_t val);
    explicit Value(int32_t val);
    explicit Value(int64_t val);
    explicit Value(uint8_t val);
    explicit Value(uint16_t val);
    explicit Value(uint32_t val);
    explicit Value(uint64_t val);
    explicit Value(float val);
    explicit Value(double val);
    explicit Value(const char *val);
    explicit Value(const std::string &val);
    explicit Value(std::string &&val);
    explicit Value(hugeint_t val);

    // Named constructors for explicitness.
    static Value BOOLEAN(bool val);
    static Value TINYINT(int8_t val);
    static Value SMALLINT(int16_t val);
    static Value INTEGER(int32_t val);
    static Value BIGINT(int64_t val);
    static Value HUGEINT(hugeint_t val);
    static Value UTINYINT(uint8_t val);
    static Value USMALLINT(uint16_t val);
    static Value UINTEGER(uint32_t val);
    static Value UBIGINT(uint64_t val);
    static Value FLOAT(float val);
    static Value DOUBLE(double val);
    static Value VARCHAR(const std::string &val);
    static Value VARCHAR(const char *val);
    static Value BLOB(const std::string &val);
    static Value LIST(std::vector<Value> values, LogicalType child_type);
    // Date / time / timestamp: physically int32 / int64. These preserve
    // the LogicalType so ToString() can render ISO strings instead of
    // raw integers — without them, every aggregation (max/min/group-by)
    // dropped the date/timestamp annotation and rendered as microseconds.
    static Value DATE(int32_t days_since_epoch);
    static Value TIMESTAMP(int64_t micros_since_epoch);
    static Value TIME(int64_t micros_since_midnight);

    // Parse 'YYYY-MM-DD HH:MM:SS[.ffffff]' into epoch microseconds. Returns
    // false on any deviation from the strict shape; on success out_micros
    // is the UTC epoch-microseconds value. Used by CSV type inference.
    static bool TryParseTimestampMicros(const char *s, size_t len, int64_t &out_micros);

    bool IsNull() const { return is_null_; }
    const LogicalType &type() const { return type_; }

    // Extract the value as a C++ type. Throws on type mismatch.
    template <class T>
    T GetValue() const;

    std::string ToString() const;

    bool operator==(const Value &other) const;
    bool operator!=(const Value &other) const { return !(*this == other); }
    bool operator<(const Value &other) const;
    bool operator>(const Value &other) const { return other < *this; }
    bool operator<=(const Value &other) const { return !(other < *this); }
    bool operator>=(const Value &other) const { return !(*this < other); }

private:
    LogicalType type_;
    bool is_null_;

    // Value storage. Using separate fields rather than a union
    // because string and vector have non-trivial destructors.
    union {
        bool boolean_;
        int8_t tinyint_;
        int16_t smallint_;
        int32_t integer_;
        int64_t bigint_;
        uint8_t utinyint_;
        uint16_t usmallint_;
        uint32_t uinteger_;
        uint64_t ubigint_;
        float float_;
        double double_;
        hugeint_t hugeint_;
    };
    std::string str_value_;
    std::vector<Value> list_value_;
};

} // namespace slothdb

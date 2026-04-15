#include "slothdb/common/types/value.hpp"
#include "slothdb/common/exception.hpp"
#include <cstring>

namespace slothdb {

Value::Value() : type_(LogicalType::SQLNULL()), is_null_(true), bigint_(0) {}

Value::Value(const Value &other)
    : type_(other.type_), is_null_(other.is_null_), str_value_(other.str_value_),
      list_value_(other.list_value_) {
    std::memcpy(&boolean_, &other.boolean_, sizeof(hugeint_));
}

Value::Value(Value &&other) noexcept
    : type_(std::move(other.type_)), is_null_(other.is_null_),
      str_value_(std::move(other.str_value_)), list_value_(std::move(other.list_value_)) {
    std::memcpy(&boolean_, &other.boolean_, sizeof(hugeint_));
}

Value &Value::operator=(const Value &other) {
    if (this != &other) {
        type_ = other.type_;
        is_null_ = other.is_null_;
        std::memcpy(&boolean_, &other.boolean_, sizeof(hugeint_));
        str_value_ = other.str_value_;
        list_value_ = other.list_value_;
    }
    return *this;
}

Value &Value::operator=(Value &&other) noexcept {
    if (this != &other) {
        type_ = std::move(other.type_);
        is_null_ = other.is_null_;
        std::memcpy(&boolean_, &other.boolean_, sizeof(hugeint_));
        str_value_ = std::move(other.str_value_);
        list_value_ = std::move(other.list_value_);
    }
    return *this;
}

Value::Value(bool val) : type_(LogicalType::BOOLEAN()), is_null_(false), boolean_(val) {}
Value::Value(int8_t val) : type_(LogicalType::TINYINT()), is_null_(false), tinyint_(val) {}
Value::Value(int16_t val) : type_(LogicalType::SMALLINT()), is_null_(false), smallint_(val) {}
Value::Value(int32_t val) : type_(LogicalType::INTEGER()), is_null_(false), integer_(val) {}
Value::Value(int64_t val) : type_(LogicalType::BIGINT()), is_null_(false), bigint_(val) {}
Value::Value(uint8_t val) : type_(LogicalType::UTINYINT()), is_null_(false), utinyint_(val) {}
Value::Value(uint16_t val) : type_(LogicalType::USMALLINT()), is_null_(false), usmallint_(val) {}
Value::Value(uint32_t val) : type_(LogicalType::UINTEGER()), is_null_(false), uinteger_(val) {}
Value::Value(uint64_t val) : type_(LogicalType::UBIGINT()), is_null_(false), ubigint_(val) {}
Value::Value(float val) : type_(LogicalType::FLOAT()), is_null_(false), float_(val) {}
Value::Value(double val) : type_(LogicalType::DOUBLE()), is_null_(false), double_(val) {}
Value::Value(hugeint_t val) : type_(LogicalType::HUGEINT()), is_null_(false), hugeint_(val) {}

Value::Value(const char *val)
    : type_(LogicalType::VARCHAR()), is_null_(false), bigint_(0), str_value_(val) {}
Value::Value(const std::string &val)
    : type_(LogicalType::VARCHAR()), is_null_(false), bigint_(0), str_value_(val) {}
Value::Value(std::string &&val)
    : type_(LogicalType::VARCHAR()), is_null_(false), bigint_(0), str_value_(std::move(val)) {}

Value Value::BOOLEAN(bool val) { return Value(val); }
Value Value::TINYINT(int8_t val) { return Value(val); }
Value Value::SMALLINT(int16_t val) { return Value(val); }
Value Value::INTEGER(int32_t val) { return Value(val); }
Value Value::BIGINT(int64_t val) { return Value(val); }
Value Value::HUGEINT(hugeint_t val) { return Value(val); }
Value Value::UTINYINT(uint8_t val) { return Value(val); }
Value Value::USMALLINT(uint16_t val) { return Value(val); }
Value Value::UINTEGER(uint32_t val) { return Value(val); }
Value Value::UBIGINT(uint64_t val) { return Value(val); }
Value Value::FLOAT(float val) { return Value(val); }
Value Value::DOUBLE(double val) { return Value(val); }
Value Value::VARCHAR(const std::string &val) { return Value(val); }
Value Value::VARCHAR(const char *val) { return Value(val); }

Value Value::BLOB(const std::string &val) {
    Value result(val);
    result.type_ = LogicalType::BLOB();
    return result;
}

Value Value::LIST(std::vector<Value> values, LogicalType child_type) {
    Value result;
    result.type_ = LogicalType::LIST(child_type);
    result.is_null_ = false;
    result.list_value_ = std::move(values);
    return result;
}

template <>
bool Value::GetValue<bool>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return boolean_;
}

template <>
int8_t Value::GetValue<int8_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return tinyint_;
}

template <>
int16_t Value::GetValue<int16_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return smallint_;
}

template <>
int32_t Value::GetValue<int32_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return integer_;
}

template <>
int64_t Value::GetValue<int64_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return bigint_;
}

template <>
uint8_t Value::GetValue<uint8_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return utinyint_;
}

template <>
uint16_t Value::GetValue<uint16_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return usmallint_;
}

template <>
uint32_t Value::GetValue<uint32_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return uinteger_;
}

template <>
uint64_t Value::GetValue<uint64_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return ubigint_;
}

template <>
float Value::GetValue<float>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return float_;
}

template <>
double Value::GetValue<double>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return double_;
}

template <>
hugeint_t Value::GetValue<hugeint_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return hugeint_;
}

template <>
std::string Value::GetValue<std::string>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return str_value_;
}

std::string Value::ToString() const {
    if (is_null_) return "NULL";
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN:
        return boolean_ ? "true" : "false";
    case LogicalTypeId::TINYINT:
        return std::to_string(tinyint_);
    case LogicalTypeId::SMALLINT:
        return std::to_string(smallint_);
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
        return std::to_string(integer_);
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIME:
        return std::to_string(bigint_);
    case LogicalTypeId::UTINYINT:
        return std::to_string(utinyint_);
    case LogicalTypeId::USMALLINT:
        return std::to_string(usmallint_);
    case LogicalTypeId::UINTEGER:
        return std::to_string(uinteger_);
    case LogicalTypeId::UBIGINT:
        return std::to_string(ubigint_);
    case LogicalTypeId::FLOAT:
        return std::to_string(float_);
    case LogicalTypeId::DOUBLE:
        return std::to_string(double_);
    case LogicalTypeId::HUGEINT:
        return hugeint_.ToString();
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB:
        return str_value_;
    default:
        return "(" + type_.ToString() + ")";
    }
}

bool Value::operator==(const Value &other) const {
    if (is_null_ && other.is_null_) return true;
    if (is_null_ != other.is_null_) return false;
    if (type_ != other.type_) return false;
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN: return boolean_ == other.boolean_;
    case LogicalTypeId::TINYINT: return tinyint_ == other.tinyint_;
    case LogicalTypeId::SMALLINT: return smallint_ == other.smallint_;
    case LogicalTypeId::INTEGER: return integer_ == other.integer_;
    case LogicalTypeId::BIGINT: return bigint_ == other.bigint_;
    case LogicalTypeId::UTINYINT: return utinyint_ == other.utinyint_;
    case LogicalTypeId::USMALLINT: return usmallint_ == other.usmallint_;
    case LogicalTypeId::UINTEGER: return uinteger_ == other.uinteger_;
    case LogicalTypeId::UBIGINT: return ubigint_ == other.ubigint_;
    case LogicalTypeId::FLOAT: return float_ == other.float_;
    case LogicalTypeId::DOUBLE: return double_ == other.double_;
    case LogicalTypeId::HUGEINT: return hugeint_ == other.hugeint_;
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB: return str_value_ == other.str_value_;
    default: return false;
    }
}

bool Value::operator<(const Value &other) const {
    if (is_null_ || other.is_null_) return false;
    if (type_ != other.type_) return false;
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN: return boolean_ < other.boolean_;
    case LogicalTypeId::TINYINT: return tinyint_ < other.tinyint_;
    case LogicalTypeId::SMALLINT: return smallint_ < other.smallint_;
    case LogicalTypeId::INTEGER: return integer_ < other.integer_;
    case LogicalTypeId::BIGINT: return bigint_ < other.bigint_;
    case LogicalTypeId::UTINYINT: return utinyint_ < other.utinyint_;
    case LogicalTypeId::USMALLINT: return usmallint_ < other.usmallint_;
    case LogicalTypeId::UINTEGER: return uinteger_ < other.uinteger_;
    case LogicalTypeId::UBIGINT: return ubigint_ < other.ubigint_;
    case LogicalTypeId::FLOAT: return float_ < other.float_;
    case LogicalTypeId::DOUBLE: return double_ < other.double_;
    case LogicalTypeId::HUGEINT: return hugeint_ < other.hugeint_;
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB: return str_value_ < other.str_value_;
    default: return false;
    }
}

} // namespace slothdb

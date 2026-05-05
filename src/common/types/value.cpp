#include "slothdb/common/types/value.hpp"
#include "slothdb/common/exception.hpp"
#include <cstdio>
#include <cstring>

namespace slothdb {

// Convert days-since-1970-01-01 to (Y, M, D) using Howard Hinnant's
// civil-from-days. Used to render DATE values as ISO-8601 strings instead
// of opaque integers.
static void DaysToYMD(int32_t days, int &y, unsigned &m, unsigned &d) {
    int z = days + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yr = static_cast<int>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = yr + (m <= 2);
}

// Format a TIMESTAMP value (microseconds since 1970-01-01) as
// "YYYY-MM-DD HH:MM:SS[.uuuuuu]". Avoids gmtime so this is reentrant
// and works the same on every platform.
static std::string FormatTimestamp(int64_t micros) {
    int64_t secs = micros / 1000000;
    int64_t us = micros - secs * 1000000;
    if (us < 0) { secs -= 1; us += 1000000; }
    int64_t days = secs / 86400;
    int64_t tod = secs - days * 86400;
    if (tod < 0) { days -= 1; tod += 86400; }
    int y; unsigned m, d;
    DaysToYMD(static_cast<int32_t>(days), y, m, d);
    int hh = static_cast<int>(tod / 3600);
    int mm = static_cast<int>((tod / 60) % 60);
    int ss = static_cast<int>(tod % 60);
    char buf[64];
    if (us == 0) {
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d:%02d",
                      y, m, d, hh, mm, ss);
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d:%02d.%06d",
                      y, m, d, hh, mm, ss, static_cast<int>(us));
    }
    return std::string(buf);
}

static std::string FormatDate(int32_t days) {
    int y; unsigned m, d;
    DaysToYMD(days, y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return std::string(buf);
}

static std::string FormatTime(int64_t micros) {
    int64_t us = micros % 1000000;
    if (us < 0) us += 1000000;
    int64_t secs = (micros - us) / 1000000;
    if (secs < 0) secs = ((secs % 86400) + 86400) % 86400;
    int hh = static_cast<int>((secs / 3600) % 24);
    int mm = static_cast<int>((secs / 60) % 60);
    int ss = static_cast<int>(secs % 60);
    char buf[32];
    if (us == 0)
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mm, ss);
    else
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d",
                      hh, mm, ss, static_cast<int>(us));
    return std::string(buf);
}

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

Value Value::DATE(int32_t days) {
    Value result(days);
    result.type_ = LogicalType::DATE();
    return result;
}

Value Value::TIMESTAMP(int64_t micros) {
    Value result(micros);
    result.type_ = LogicalType::TIMESTAMP();
    return result;
}

// Days from civil 1970-01-01 to civil (y,m,d). Inverse of DaysToYMD above.
// Algorithm by Howard Hinnant; valid for any proleptic Gregorian date.
static int32_t DaysFromYMD(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

bool Value::TryParseTimestampMicros(const char *s, size_t len, int64_t &out_micros) {
    // Strict: 'YYYY-MM-DD HH:MM:SS' (19 chars) or with '.ffffff' suffix (1-6 digits).
    if (len < 19) return false;
    auto digit = [&](size_t i) { return s[i] >= '0' && s[i] <= '9'; };
    static const size_t kDigitPos[] = {0,1,2,3, 5,6, 8,9, 11,12, 14,15, 17,18};
    for (size_t i : kDigitPos)
        if (!digit(i)) return false;
    if (s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':')
        return false;
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    unsigned mo = (s[5]-'0')*10 + (s[6]-'0');
    unsigned d  = (s[8]-'0')*10 + (s[9]-'0');
    unsigned hh = (s[11]-'0')*10 + (s[12]-'0');
    unsigned mm = (s[14]-'0')*10 + (s[15]-'0');
    unsigned ss = (s[17]-'0')*10 + (s[18]-'0');
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 59)
        return false;
    int64_t frac_us = 0;
    if (len > 19) {
        if (s[19] != '.') return false;
        size_t fd = 0;
        while (19 + 1 + fd < len && fd < 6 && digit(19 + 1 + fd)) {
            frac_us = frac_us * 10 + (s[19 + 1 + fd] - '0');
            fd++;
        }
        if (fd == 0 || 19 + 1 + fd != len) return false;
        for (size_t i = fd; i < 6; i++) frac_us *= 10;
    }
    int64_t days = DaysFromYMD(y, mo, d);
    int64_t secs = days * 86400 + static_cast<int64_t>(hh) * 3600
                 + static_cast<int64_t>(mm) * 60 + static_cast<int64_t>(ss);
    out_micros = secs * 1000000 + frac_us;
    return true;
}

bool Value::TryParseDateStringEpochDays(const char *s, size_t len, int32_t &out_days) {
    if (len != 10) return false;
    auto digit = [&](size_t i) { return s[i] >= '0' && s[i] <= '9'; };
    if (!digit(0) || !digit(1) || !digit(2) || !digit(3) ||
        !digit(5) || !digit(6) || !digit(8) || !digit(9)) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    unsigned mo = (s[5]-'0')*10 + (s[6]-'0');
    unsigned d  = (s[8]-'0')*10 + (s[9]-'0');
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    out_days = DaysFromYMD(y, mo, d);
    return true;
}

Value Value::TIME(int64_t micros) {
    Value result(micros);
    result.type_ = LogicalType::TIME();
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
        return std::to_string(integer_);
    case LogicalTypeId::DATE:
        return FormatDate(integer_);
    case LogicalTypeId::BIGINT:
        return std::to_string(bigint_);
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
        return FormatTimestamp(bigint_);
    case LogicalTypeId::TIME:
        return FormatTime(bigint_);
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

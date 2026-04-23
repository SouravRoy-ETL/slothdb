#pragma once

#include "slothdb/common/constants.hpp"
#include "slothdb/common/enums.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

enum class LogicalTypeId : uint8_t {
    INVALID,
    SQLNULL,
    BOOLEAN,
    TINYINT,
    SMALLINT,
    INTEGER,
    BIGINT,
    HUGEINT,
    UTINYINT,
    USMALLINT,
    UINTEGER,
    UBIGINT,
    FLOAT,
    DOUBLE,
    DECIMAL,
    VARCHAR,
    BLOB,
    DATE,
    TIME,
    TIMESTAMP,
    TIMESTAMP_TZ,
    INTERVAL,
    UUID,
    ENUM,
    LIST,
    ARRAY,
    MAP,
    STRUCT,
    UNION,
    ANY
};

// Base class for parameterized type info (DECIMAL, LIST, STRUCT, etc.)
class ExtraTypeInfo {
public:
    virtual ~ExtraTypeInfo() = default;
    virtual bool Equals(const ExtraTypeInfo &other) const = 0;
};

class DecimalTypeInfo : public ExtraTypeInfo {
public:
    DecimalTypeInfo(uint8_t width, uint8_t scale) : width_(width), scale_(scale) {}
    uint8_t Width() const { return width_; }
    uint8_t Scale() const { return scale_; }
    bool Equals(const ExtraTypeInfo &other) const override;

private:
    uint8_t width_;
    uint8_t scale_;
};

// Carries the declared maximum length for VARCHAR(n) / CHAR(n).
class VarcharTypeInfo : public ExtraTypeInfo {
public:
    explicit VarcharTypeInfo(idx_t max_length) : max_length_(max_length) {}
    idx_t MaxLength() const { return max_length_; }
    bool Equals(const ExtraTypeInfo &other) const override;

private:
    idx_t max_length_;
};

class ListTypeInfo : public ExtraTypeInfo {
public:
    explicit ListTypeInfo(LogicalTypeId child_type);
    // The actual child LogicalType is set after construction via SetChildType().
    // This avoids circular dependency with LogicalType.
    bool Equals(const ExtraTypeInfo &other) const override;

    LogicalTypeId child_type_id_;
    std::shared_ptr<ExtraTypeInfo> child_info_;
};

struct StructField {
    std::string name;
    LogicalTypeId type_id;
    std::shared_ptr<ExtraTypeInfo> type_info;
};

class StructTypeInfo : public ExtraTypeInfo {
public:
    explicit StructTypeInfo(std::vector<StructField> fields) : fields_(std::move(fields)) {}
    const std::vector<StructField> &Fields() const { return fields_; }
    bool Equals(const ExtraTypeInfo &other) const override;

private:
    std::vector<StructField> fields_;
};

class ArrayTypeInfo : public ExtraTypeInfo {
public:
    ArrayTypeInfo(LogicalTypeId child_type, idx_t size)
        : child_type_id_(child_type), size_(size) {}
    idx_t Size() const { return size_; }
    bool Equals(const ExtraTypeInfo &other) const override;

    LogicalTypeId child_type_id_;
    std::shared_ptr<ExtraTypeInfo> child_info_;
    idx_t size_;
};

// The core type class. Stores a LogicalTypeId + optional ExtraTypeInfo for parameterized types.
class LogicalType {
public:
    LogicalType() : id_(LogicalTypeId::INVALID) {}
    explicit LogicalType(LogicalTypeId id) : id_(id) {}
    LogicalType(LogicalTypeId id, std::shared_ptr<ExtraTypeInfo> info)
        : id_(id), info_(std::move(info)) {}

    LogicalTypeId id() const { return id_; }
    const ExtraTypeInfo *GetExtraInfo() const { return info_.get(); }

    // Maps this logical type to the physical C++ storage type.
    PhysicalType GetInternalType() const;

    std::string ToString() const;

    bool operator==(const LogicalType &other) const;
    bool operator!=(const LogicalType &other) const { return !(*this == other); }

    // Convenience static factory methods for common types.
    static LogicalType BOOLEAN() { return LogicalType(LogicalTypeId::BOOLEAN); }
    static LogicalType TINYINT() { return LogicalType(LogicalTypeId::TINYINT); }
    static LogicalType SMALLINT() { return LogicalType(LogicalTypeId::SMALLINT); }
    static LogicalType INTEGER() { return LogicalType(LogicalTypeId::INTEGER); }
    static LogicalType BIGINT() { return LogicalType(LogicalTypeId::BIGINT); }
    static LogicalType HUGEINT() { return LogicalType(LogicalTypeId::HUGEINT); }
    static LogicalType UTINYINT() { return LogicalType(LogicalTypeId::UTINYINT); }
    static LogicalType USMALLINT() { return LogicalType(LogicalTypeId::USMALLINT); }
    static LogicalType UINTEGER() { return LogicalType(LogicalTypeId::UINTEGER); }
    static LogicalType UBIGINT() { return LogicalType(LogicalTypeId::UBIGINT); }
    static LogicalType FLOAT() { return LogicalType(LogicalTypeId::FLOAT); }
    static LogicalType DOUBLE() { return LogicalType(LogicalTypeId::DOUBLE); }
    static LogicalType VARCHAR() { return LogicalType(LogicalTypeId::VARCHAR); }
    static LogicalType BLOB() { return LogicalType(LogicalTypeId::BLOB); }
    static LogicalType DATE() { return LogicalType(LogicalTypeId::DATE); }
    static LogicalType TIME() { return LogicalType(LogicalTypeId::TIME); }
    static LogicalType TIMESTAMP() { return LogicalType(LogicalTypeId::TIMESTAMP); }
    static LogicalType INTERVAL() { return LogicalType(LogicalTypeId::INTERVAL); }
    static LogicalType UUID() { return LogicalType(LogicalTypeId::UUID); }
    static LogicalType SQLNULL() { return LogicalType(LogicalTypeId::SQLNULL); }
    static LogicalType ANY() { return LogicalType(LogicalTypeId::ANY); }

    static LogicalType DECIMAL(uint8_t width, uint8_t scale);
    static LogicalType VARCHAR_N(idx_t max_length);
    static LogicalType LIST(const LogicalType &child);
    static LogicalType STRUCT(std::vector<StructField> fields);
    static LogicalType ARRAY(const LogicalType &child, idx_t size);

private:
    LogicalTypeId id_;
    std::shared_ptr<ExtraTypeInfo> info_;
};

// Returns string name of a LogicalTypeId.
std::string LogicalTypeIdToString(LogicalTypeId id);

} // namespace slothdb

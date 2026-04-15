#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/exception.hpp"

namespace slothdb {

// --- ExtraTypeInfo Equals implementations ---

bool DecimalTypeInfo::Equals(const ExtraTypeInfo &other) const {
    auto *o = dynamic_cast<const DecimalTypeInfo *>(&other);
    return o && width_ == o->width_ && scale_ == o->scale_;
}

ListTypeInfo::ListTypeInfo(LogicalTypeId child_type)
    : child_type_id_(child_type) {}

bool ListTypeInfo::Equals(const ExtraTypeInfo &other) const {
    auto *o = dynamic_cast<const ListTypeInfo *>(&other);
    return o && child_type_id_ == o->child_type_id_;
}

bool StructTypeInfo::Equals(const ExtraTypeInfo &other) const {
    auto *o = dynamic_cast<const StructTypeInfo *>(&other);
    if (!o || fields_.size() != o->fields_.size()) return false;
    for (size_t i = 0; i < fields_.size(); i++) {
        if (fields_[i].name != o->fields_[i].name) return false;
        if (fields_[i].type_id != o->fields_[i].type_id) return false;
    }
    return true;
}

bool ArrayTypeInfo::Equals(const ExtraTypeInfo &other) const {
    auto *o = dynamic_cast<const ArrayTypeInfo *>(&other);
    return o && child_type_id_ == o->child_type_id_ && size_ == o->size_;
}

// --- LogicalType ---

PhysicalType LogicalType::GetInternalType() const {
    switch (id_) {
    case LogicalTypeId::BOOLEAN:
        return PhysicalType::BOOL;
    case LogicalTypeId::TINYINT:
        return PhysicalType::INT8;
    case LogicalTypeId::SMALLINT:
        return PhysicalType::INT16;
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
        return PhysicalType::INT32;
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
    case LogicalTypeId::TIME:
        return PhysicalType::INT64;
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::UUID:
        return PhysicalType::INT128;
    case LogicalTypeId::UTINYINT:
        return PhysicalType::UINT8;
    case LogicalTypeId::USMALLINT:
        return PhysicalType::UINT16;
    case LogicalTypeId::UINTEGER:
        return PhysicalType::UINT32;
    case LogicalTypeId::UBIGINT:
        return PhysicalType::UINT64;
    case LogicalTypeId::FLOAT:
        return PhysicalType::FLOAT;
    case LogicalTypeId::DOUBLE:
        return PhysicalType::DOUBLE;
    case LogicalTypeId::DECIMAL: {
        // DECIMAL physical type depends on width.
        auto *info = dynamic_cast<const DecimalTypeInfo *>(info_.get());
        if (!info) return PhysicalType::INT128;
        if (info->Width() <= 4) return PhysicalType::INT16;
        if (info->Width() <= 9) return PhysicalType::INT32;
        if (info->Width() <= 18) return PhysicalType::INT64;
        return PhysicalType::INT128;
    }
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB:
    case LogicalTypeId::ENUM:
        return PhysicalType::VARCHAR;
    case LogicalTypeId::INTERVAL:
        return PhysicalType::INTERVAL;
    case LogicalTypeId::STRUCT:
    case LogicalTypeId::MAP:
    case LogicalTypeId::UNION:
        return PhysicalType::STRUCT;
    case LogicalTypeId::LIST:
        return PhysicalType::LIST;
    case LogicalTypeId::ARRAY:
        return PhysicalType::ARRAY;
    case LogicalTypeId::SQLNULL:
    case LogicalTypeId::ANY:
    case LogicalTypeId::INVALID:
        return PhysicalType::INVALID;
    }
    return PhysicalType::INVALID;
}

std::string LogicalType::ToString() const {
    switch (id_) {
    case LogicalTypeId::DECIMAL: {
        auto *info = dynamic_cast<const DecimalTypeInfo *>(info_.get());
        if (info) {
            return "DECIMAL(" + std::to_string(info->Width()) + "," +
                   std::to_string(info->Scale()) + ")";
        }
        return "DECIMAL";
    }
    default:
        return LogicalTypeIdToString(id_);
    }
}

bool LogicalType::operator==(const LogicalType &other) const {
    if (id_ != other.id_) return false;
    if (info_ && other.info_) return info_->Equals(*other.info_);
    return info_ == other.info_; // both null
}

LogicalType LogicalType::DECIMAL(uint8_t width, uint8_t scale) {
    return LogicalType(LogicalTypeId::DECIMAL, std::make_shared<DecimalTypeInfo>(width, scale));
}

LogicalType LogicalType::LIST(const LogicalType &child) {
    auto info = std::make_shared<ListTypeInfo>(child.id());
    info->child_info_ = nullptr; // simplified for now
    return LogicalType(LogicalTypeId::LIST, std::move(info));
}

LogicalType LogicalType::STRUCT(std::vector<StructField> fields) {
    return LogicalType(LogicalTypeId::STRUCT, std::make_shared<StructTypeInfo>(std::move(fields)));
}

LogicalType LogicalType::ARRAY(const LogicalType &child, idx_t size) {
    auto info = std::make_shared<ArrayTypeInfo>(child.id(), size);
    return LogicalType(LogicalTypeId::ARRAY, std::move(info));
}

std::string LogicalTypeIdToString(LogicalTypeId id) {
    switch (id) {
    case LogicalTypeId::INVALID: return "INVALID";
    case LogicalTypeId::SQLNULL: return "NULL";
    case LogicalTypeId::BOOLEAN: return "BOOLEAN";
    case LogicalTypeId::TINYINT: return "TINYINT";
    case LogicalTypeId::SMALLINT: return "SMALLINT";
    case LogicalTypeId::INTEGER: return "INTEGER";
    case LogicalTypeId::BIGINT: return "BIGINT";
    case LogicalTypeId::HUGEINT: return "HUGEINT";
    case LogicalTypeId::UTINYINT: return "UTINYINT";
    case LogicalTypeId::USMALLINT: return "USMALLINT";
    case LogicalTypeId::UINTEGER: return "UINTEGER";
    case LogicalTypeId::UBIGINT: return "UBIGINT";
    case LogicalTypeId::FLOAT: return "FLOAT";
    case LogicalTypeId::DOUBLE: return "DOUBLE";
    case LogicalTypeId::DECIMAL: return "DECIMAL";
    case LogicalTypeId::VARCHAR: return "VARCHAR";
    case LogicalTypeId::BLOB: return "BLOB";
    case LogicalTypeId::DATE: return "DATE";
    case LogicalTypeId::TIME: return "TIME";
    case LogicalTypeId::TIMESTAMP: return "TIMESTAMP";
    case LogicalTypeId::TIMESTAMP_TZ: return "TIMESTAMP WITH TIME ZONE";
    case LogicalTypeId::INTERVAL: return "INTERVAL";
    case LogicalTypeId::UUID: return "UUID";
    case LogicalTypeId::ENUM: return "ENUM";
    case LogicalTypeId::LIST: return "LIST";
    case LogicalTypeId::ARRAY: return "ARRAY";
    case LogicalTypeId::MAP: return "MAP";
    case LogicalTypeId::STRUCT: return "STRUCT";
    case LogicalTypeId::UNION: return "UNION";
    case LogicalTypeId::ANY: return "ANY";
    }
    return "UNKNOWN";
}

} // namespace slothdb

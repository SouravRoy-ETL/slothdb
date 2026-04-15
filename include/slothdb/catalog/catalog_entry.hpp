#pragma once

#include <string>
#include <cstdint>

namespace slothdb {

enum class CatalogEntryType : uint8_t {
    SCHEMA,
    TABLE,
    VIEW,
    INDEX,
    SCALAR_FUNCTION,
    AGGREGATE_FUNCTION,
    TABLE_FUNCTION,
    TYPE,
    INVALID
};

// Base class for all entries in the catalog.
class CatalogEntry {
public:
    CatalogEntry(CatalogEntryType type, const std::string &name)
        : type_(type), name_(name) {}
    virtual ~CatalogEntry() = default;

    CatalogEntryType GetType() const { return type_; }
    const std::string &GetName() const { return name_; }

private:
    CatalogEntryType type_;
    std::string name_;
};

} // namespace slothdb

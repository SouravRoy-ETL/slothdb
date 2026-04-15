#pragma once

#include "slothdb/catalog/catalog_entry.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

class DataTable;

// Describes a column in a table.
struct ColumnDefinition {
    std::string name;
    LogicalType type;

    ColumnDefinition(const std::string &name, const LogicalType &type)
        : name(name), type(type) {}
};

// Catalog entry for a table. Owns the column definitions and
// a reference to the physical DataTable.
class TableCatalogEntry : public CatalogEntry {
public:
    TableCatalogEntry(const std::string &schema, const std::string &name,
                      std::vector<ColumnDefinition> columns);

    const std::string &GetSchema() const { return schema_; }
    const std::vector<ColumnDefinition> &GetColumns() const { return columns_; }
    idx_t ColumnCount() const { return static_cast<idx_t>(columns_.size()); }

    // Get column index by name. Returns INVALID_INDEX if not found.
    idx_t GetColumnIndex(const std::string &col_name) const;

    // Get the column types as a vector.
    std::vector<LogicalType> GetTypes() const;

    // The physical storage for this table.
    DataTable &GetStorage() { return *storage_; }
    const DataTable &GetStorage() const { return *storage_; }
    void SetStorage(std::shared_ptr<DataTable> storage) { storage_ = std::move(storage); }

private:
    std::string schema_;
    std::vector<ColumnDefinition> columns_;
    std::shared_ptr<DataTable> storage_;
};

} // namespace slothdb

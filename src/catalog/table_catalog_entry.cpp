#include "slothdb/catalog/table_catalog_entry.hpp"

namespace slothdb {

TableCatalogEntry::TableCatalogEntry(const std::string &schema, const std::string &name,
                                     std::vector<ColumnDefinition> columns)
    : CatalogEntry(CatalogEntryType::TABLE, name),
      schema_(schema), columns_(std::move(columns)) {}

idx_t TableCatalogEntry::GetColumnIndex(const std::string &col_name) const {
    for (idx_t i = 0; i < columns_.size(); i++) {
        if (columns_[i].name == col_name) {
            return i;
        }
    }
    return INVALID_INDEX;
}

std::vector<LogicalType> TableCatalogEntry::GetTypes() const {
    std::vector<LogicalType> types;
    types.reserve(columns_.size());
    for (auto &col : columns_) {
        types.push_back(col.type);
    }
    return types;
}

} // namespace slothdb

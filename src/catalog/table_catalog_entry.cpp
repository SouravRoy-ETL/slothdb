#include "slothdb/catalog/table_catalog_entry.hpp"
#include "slothdb/storage/data_table.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/exception.hpp"
#include <unordered_set>

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

void TableCatalogEntry::CheckPrimaryKeyForChunk(DataChunk &chunk) const {
    auto pk = GetPrimaryKeyColumns();
    if (pk.empty() || !storage_) return;

    // Build a composite key string for a row (handles multi-column PK).
    auto row_key = [&](DataChunk &c, idx_t row) -> std::string {
        std::string k;
        for (idx_t ci : pk) {
            k += c.GetValue(ci, row).ToString();
            k.push_back('\x1f'); // unit separator between key parts
        }
        return k;
    };

    // Collect existing PK values from storage.
    std::unordered_set<std::string> seen;
    if (storage_->Count() > 0) {
        auto state = storage_->InitScan();
        DataChunk scan;
        scan.Initialize(storage_->GetTypes());
        while (storage_->Scan(state, scan)) {
            for (idx_t r = 0; r < scan.size(); r++) {
                seen.insert(row_key(scan, r));
            }
            scan.Reset();
        }
    }

    // Check the incoming chunk against storage and within itself.
    for (idx_t r = 0; r < chunk.size(); r++) {
        auto key = row_key(chunk, r);
        if (!seen.insert(key).second) {
            std::string col_names;
            for (idx_t ci : pk) {
                if (!col_names.empty()) col_names += ", ";
                col_names += columns_[ci].name;
            }
            throw ConstraintException(
                "UNIQUE constraint violation: duplicate value for PRIMARY KEY (" +
                col_names + ")");
        }
    }
}

} // namespace slothdb

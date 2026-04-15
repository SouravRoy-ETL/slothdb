#pragma once

#include "slothdb/catalog/catalog_entry.hpp"
#include "slothdb/catalog/table_catalog_entry.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace slothdb {

// A schema (namespace) in the catalog. Owns table entries.
class SchemaCatalogEntry : public CatalogEntry {
public:
    explicit SchemaCatalogEntry(const std::string &name);

    // Create a table in this schema. Throws if table already exists.
    TableCatalogEntry &CreateTable(const std::string &name,
                                   std::vector<ColumnDefinition> columns);

    // Look up a table by name. Returns nullptr if not found.
    TableCatalogEntry *GetTable(const std::string &name);

    // Check if a table exists.
    bool TableExists(const std::string &name) const;

    // Drop a table. Returns true if dropped, false if not found.
    bool DropTable(const std::string &name);

    // Get all table names.
    std::vector<std::string> GetTableNames() const;

private:
    std::unordered_map<std::string, std::unique_ptr<TableCatalogEntry>> tables_;
};

} // namespace slothdb

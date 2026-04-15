#pragma once

#include "slothdb/catalog/schema_catalog_entry.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace slothdb {

// The top-level catalog. Manages schemas, tables, and other metadata.
// Thread-safe via mutex.
class Catalog {
public:
    static constexpr const char *DEFAULT_SCHEMA = "main";

    Catalog();

    // Schema operations.
    SchemaCatalogEntry &GetSchema(const std::string &name = DEFAULT_SCHEMA);
    SchemaCatalogEntry &CreateSchema(const std::string &name);
    bool SchemaExists(const std::string &name) const;

    // Convenience: table operations on the default schema.
    TableCatalogEntry &CreateTable(const std::string &name,
                                   std::vector<ColumnDefinition> columns,
                                   const std::string &schema = DEFAULT_SCHEMA);

    TableCatalogEntry *GetTable(const std::string &name,
                                const std::string &schema = DEFAULT_SCHEMA);

    bool DropTable(const std::string &name,
                   const std::string &schema = DEFAULT_SCHEMA);

private:
    mutable std::mutex lock_;
    std::unordered_map<std::string, std::unique_ptr<SchemaCatalogEntry>> schemas_;
};

} // namespace slothdb

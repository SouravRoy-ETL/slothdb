#include "slothdb/catalog/catalog.hpp"
#include "slothdb/common/exception.hpp"

namespace slothdb {

Catalog::Catalog() {
    // Create the default "main" schema.
    schemas_[DEFAULT_SCHEMA] = std::make_unique<SchemaCatalogEntry>(DEFAULT_SCHEMA);
}

SchemaCatalogEntry &Catalog::GetSchema(const std::string &name) {
    std::lock_guard<std::mutex> guard(lock_);
    auto it = schemas_.find(name);
    if (it == schemas_.end()) {
        throw CatalogException("Schema '" + name + "' not found");
    }
    return *it->second;
}

SchemaCatalogEntry &Catalog::CreateSchema(const std::string &name) {
    std::lock_guard<std::mutex> guard(lock_);
    if (schemas_.count(name)) {
        throw CatalogException("Schema '" + name + "' already exists");
    }
    auto entry = std::make_unique<SchemaCatalogEntry>(name);
    auto *ptr = entry.get();
    schemas_[name] = std::move(entry);
    return *ptr;
}

bool Catalog::SchemaExists(const std::string &name) const {
    std::lock_guard<std::mutex> guard(lock_);
    return schemas_.count(name) > 0;
}

TableCatalogEntry &Catalog::CreateTable(const std::string &name,
                                         std::vector<ColumnDefinition> columns,
                                         const std::string &schema) {
    std::lock_guard<std::mutex> guard(lock_);
    auto it = schemas_.find(schema);
    if (it == schemas_.end()) {
        throw CatalogException("Schema '" + schema + "' not found");
    }
    return it->second->CreateTable(name, std::move(columns));
}

TableCatalogEntry *Catalog::GetTable(const std::string &name,
                                      const std::string &schema) {
    std::lock_guard<std::mutex> guard(lock_);
    auto it = schemas_.find(schema);
    if (it == schemas_.end()) return nullptr;
    return it->second->GetTable(name);
}

bool Catalog::DropTable(const std::string &name, const std::string &schema) {
    std::lock_guard<std::mutex> guard(lock_);
    auto it = schemas_.find(schema);
    if (it == schemas_.end()) return false;
    return it->second->DropTable(name);
}

} // namespace slothdb

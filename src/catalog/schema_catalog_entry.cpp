#include "slothdb/catalog/schema_catalog_entry.hpp"
#include "slothdb/common/exception.hpp"

namespace slothdb {

SchemaCatalogEntry::SchemaCatalogEntry(const std::string &name)
    : CatalogEntry(CatalogEntryType::SCHEMA, name) {}

TableCatalogEntry &SchemaCatalogEntry::CreateTable(const std::string &name,
                                                    std::vector<ColumnDefinition> columns) {
    if (tables_.count(name)) {
        throw CatalogException("Table '" + name + "' already exists");
    }
    auto entry = std::make_unique<TableCatalogEntry>(GetName(), name, std::move(columns));
    auto *ptr = entry.get();
    tables_[name] = std::move(entry);
    return *ptr;
}

TableCatalogEntry *SchemaCatalogEntry::GetTable(const std::string &name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) return nullptr;
    return it->second.get();
}

bool SchemaCatalogEntry::TableExists(const std::string &name) const {
    return tables_.count(name) > 0;
}

bool SchemaCatalogEntry::DropTable(const std::string &name) {
    return tables_.erase(name) > 0;
}

std::vector<std::string> SchemaCatalogEntry::GetTableNames() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (auto &pair : tables_) {
        names.push_back(pair.first);
    }
    return names;
}

} // namespace slothdb

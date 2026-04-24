#pragma once

#include "slothdb/main/database.hpp"
#include "slothdb/catalog/table_catalog_entry.hpp"
#include "slothdb/storage/data_table.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

// Query result: holds the result of a SQL query.
struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<LogicalType> column_types;
    std::vector<std::vector<Value>> rows;

    idx_t RowCount() const { return static_cast<idx_t>(rows.size()); }
    idx_t ColumnCount() const { return static_cast<idx_t>(column_names.size()); }

    // Get a single value.
    const Value &GetValue(idx_t row, idx_t col) const { return rows[row][col]; }

    std::string ToString() const;
};

// A connection to a SlothDB database. Provides the user-facing API.
// Multiple connections can share one Database.
class Connection {
public:
    explicit Connection(Database &database);
    ~Connection();

    // ========================================================================
    // SQL Execution - the primary interface.
    // Full pipeline: parse -> bind -> plan -> execute -> results.
    // ========================================================================
    QueryResult Query(const std::string &sql);

    // DDL operations (programmatic API).
    void CreateTable(const std::string &name, std::vector<ColumnDefinition> columns);
    void DropTable(const std::string &name);

    // Data operations (programmatic API).
    void Append(const std::string &table_name, DataChunk &chunk);

    // Scan a full table. Calls the callback for each chunk.
    void Scan(const std::string &table_name,
              std::function<void(DataChunk &)> callback);

    // Transaction control (programmatic API).
    void BeginTransaction();
    void CommitTransaction();
    void RollbackTransaction();
    bool InTransaction() const { return active_txn_ != nullptr; }

    // Access to internals.
    Database &GetDatabase() { return db_; }
    Catalog &GetCatalog() { return db_.GetCatalog(); }

private:
    Database &db_;
    Transaction *active_txn_ = nullptr;
};

} // namespace slothdb

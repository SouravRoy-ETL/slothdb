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
//
// Two row representations live here:
//
// - `chunks`: typed vectors straight from the executor. Populated by SELECT
//   queries; the C API's typed-batch fetch reads directly from this without
//   ever materialising Value boxes. Avoiding the boxing is the difference
//   between 18 s and 6 s on a 10M-row window result returning to Python.
//
// - `rows`: vector<vector<Value>>. Populated lazily on first per-cell access
//   (GetValue) or on the legacy per-row API. Some non-SELECT paths still
//   write here directly.
struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<LogicalType> column_types;
    std::vector<std::vector<Value>> rows;
    std::vector<DataChunk> chunks;
    // Cumulative chunk_starts[i] = total rows in chunks[0..i-1]. Used to
    // map a row index to (chunk_idx, in_chunk_idx) in O(log N).
    mutable std::vector<idx_t> chunk_starts;
    mutable bool chunk_index_built = false;

    idx_t RowCount() const {
        if (!rows.empty()) return static_cast<idx_t>(rows.size());
        idx_t total = 0;
        for (auto &c : chunks) total += c.size();
        return total;
    }
    idx_t ColumnCount() const { return static_cast<idx_t>(column_names.size()); }

    // Build the chunk-index lookup table. Idempotent.
    void BuildChunkIndex() const {
        if (chunk_index_built) return;
        chunk_starts.clear();
        chunk_starts.reserve(chunks.size() + 1);
        idx_t total = 0;
        for (auto &c : chunks) {
            chunk_starts.push_back(total);
            total += c.size();
        }
        chunk_starts.push_back(total);
        chunk_index_built = true;
    }

    // Materialise chunks into rows. One-shot — once built, subsequent
    // GetValue calls hit the boxed copy. Callers that want to avoid this
    // (typed-batch fetch) should read from `chunks` directly.
    void MaterialiseRows() const {
        if (!rows.empty() || chunks.empty()) return;
        idx_t ncols = ColumnCount();
        idx_t total = 0;
        for (auto &c : chunks) total += c.size();
        auto &mut_rows = const_cast<std::vector<std::vector<Value>>&>(rows);
        mut_rows.reserve(total);
        for (auto &c : chunks) {
            idx_t n = c.size();
            for (idx_t i = 0; i < n; i++) {
                std::vector<Value> r;
                r.reserve(ncols);
                for (idx_t k = 0; k < ncols; k++) r.push_back(c.GetValue(k, i));
                mut_rows.push_back(std::move(r));
            }
        }
    }

    // Per-cell access. Returns by const-reference. The first call after a
    // chunk-only SELECT pays a one-time materialisation; the typed-batch C
    // API path bypasses this entirely by reading chunks itself.
    const Value &GetValue(idx_t row, idx_t col) const {
        if (rows.empty() && !chunks.empty()) MaterialiseRows();
        return rows[row][col];
    }

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

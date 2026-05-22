#pragma once

#include "slothdb/catalog/catalog_entry.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

class DataTable;
class ParquetReader;

// Describes a column in a table.
struct ColumnDefinition {
    std::string name;
    LogicalType type;
    bool not_null = false;
    bool is_primary_key = false;

    ColumnDefinition(const std::string &name, const LogicalType &type)
        : name(name), type(type) {}
    ColumnDefinition(const std::string &name, const LogicalType &type, bool not_null_)
        : name(name), type(type), not_null(not_null_) {}
    ColumnDefinition(const std::string &name, const LogicalType &type,
                     bool not_null_, bool is_pk_)
        : name(name), type(type), not_null(not_null_), is_primary_key(is_pk_) {}
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

    // Indices of PRIMARY KEY columns (for uniqueness enforcement).
    std::vector<idx_t> GetPrimaryKeyColumns() const {
        std::vector<idx_t> pk;
        for (idx_t i = 0; i < columns_.size(); i++) {
            if (columns_[i].is_primary_key) pk.push_back(i);
        }
        return pk;
    }

    // Throw ConstraintException if appending `chunk` would create a
    // duplicate PRIMARY KEY value — against existing storage rows or
    // among rows within `chunk` itself. No-op when the table has no
    // primary key or no storage. Only called from INSERT paths.
    void CheckPrimaryKeyForChunk(class DataChunk &chunk) const;

    // Get the column types as a vector.
    std::vector<LogicalType> GetTypes() const;

    // The physical storage for this table.
    DataTable &GetStorage() { return *storage_; }
    const DataTable &GetStorage() const { return *storage_; }
    void SetStorage(std::shared_ptr<DataTable> storage) { storage_ = std::move(storage); }

    // Virtual view support: stores the original SQL query for re-execution.
    bool IsView() const { return is_view_; }
    void SetViewQuery(const std::string &sql) { view_query_ = sql; is_view_ = true; }
    const std::string &GetViewQuery() const { return view_query_; }

    // Live view support: view's storage is cached; expansion in
    // Connection::Query checks the watched file's mtime and only re-executes
    // when it changes. Cheaper than re-executing on every SELECT, and still
    // reflects file mutations.
    bool IsLiveView() const { return is_live_view_; }
    void MarkLiveView() { is_live_view_ = true; }
    void SetWatchedFile(const std::string &path) { watched_file_path_ = path; }
    const std::string &GetWatchedFile() const { return watched_file_path_; }
    int64_t GetCachedMTime() const { return cached_mtime_; }
    void SetCachedMTime(int64_t v) { cached_mtime_ = v; }
    bool HasCache() const { return cache_valid_; }
    void MarkCacheValid() { cache_valid_ = true; }
    void InvalidateCache() { cache_valid_ = false; }

    // v2: incremental append. When the live view is a simple
    // `SELECT * FROM '<csv>'` and the file grows without the leading
    // bytes changing, parse only the new tail and append to storage
    // instead of re-parsing the whole file.
    bool IsIncrementalEligible() const { return incremental_eligible_; }
    void MarkIncrementalEligible(char delim) {
        incremental_eligible_ = true;
        incremental_delim_ = delim;
    }
    char GetIncrementalDelim() const { return incremental_delim_; }
    int64_t GetLastSize() const { return last_size_; }
    void SetLastSize(int64_t s) { last_size_ = s; }
    const std::string &GetFirstBytes() const { return first_bytes_; }
    void SetFirstBytes(std::string b) { first_bytes_ = std::move(b); }

    // File scan support: stores file path for streaming reads.
    bool IsFileScan() const { return !file_path_.empty(); }
    void SetFilePath(const std::string &path, char delim = ',') { file_path_ = path; file_delimiter_ = delim; file_format_ = "csv"; }
    void SetParquetPath(const std::string &path) { file_path_ = path; file_format_ = "parquet"; }
    void SetJsonPath(const std::string &path) { file_path_ = path; file_format_ = "json"; }
    void SetAvroPath(const std::string &path) { file_path_ = path; file_format_ = "avro"; }
    void SetArrowPath(const std::string &path) { file_path_ = path; file_format_ = "arrow"; }
    void SetSQLitePath(const std::string &path, const std::string &table_name) {
        file_path_ = path; file_format_ = "sqlite"; file_subname_ = table_name;
    }
    const std::string &GetFilePath() const { return file_path_; }
    char GetFileDelimiter() const { return file_delimiter_; }
    const std::string &GetFileFormat() const { return file_format_; }
    const std::string &GetFileSubname() const { return file_subname_; }

    // Optional cached ParquetReader shared across queries - the schema-
    // detection path in Connection::Query opens one to populate the catalog,
    // and we stash it here so PhysicalParquetScan::Init() reuses instead of
    // re-parsing the Thrift footer on every query (~10-20ms per query).
    void SetCachedParquetReader(std::shared_ptr<ParquetReader> r) {
        cached_parquet_reader_ = std::move(r);
    }
    std::shared_ptr<ParquetReader> GetCachedParquetReader() const {
        return cached_parquet_reader_;
    }

private:
    std::string schema_;
    std::vector<ColumnDefinition> columns_;
    std::shared_ptr<DataTable> storage_;
    bool is_view_ = false;
    bool is_live_view_ = false;
    bool cache_valid_ = false;
    bool incremental_eligible_ = false;
    char incremental_delim_ = ',';
    std::string view_query_;
    std::string watched_file_path_;
    int64_t cached_mtime_ = 0;
    int64_t last_size_ = 0;
    std::string first_bytes_;
    std::string file_path_;
    char file_delimiter_ = ',';
    std::string file_format_ = "csv";
    std::string file_subname_; // e.g. SQLite table name inside a .db file
    std::shared_ptr<ParquetReader> cached_parquet_reader_;
};

} // namespace slothdb

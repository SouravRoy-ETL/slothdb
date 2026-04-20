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

    // Virtual view support: stores the original SQL query for re-execution.
    bool IsView() const { return is_view_; }
    void SetViewQuery(const std::string &sql) { view_query_ = sql; is_view_ = true; }
    const std::string &GetViewQuery() const { return view_query_; }

    // File scan support: stores file path for streaming reads.
    bool IsFileScan() const { return !file_path_.empty(); }
    void SetFilePath(const std::string &path, char delim = ',') { file_path_ = path; file_delimiter_ = delim; file_format_ = "csv"; }
    void SetParquetPath(const std::string &path) { file_path_ = path; file_format_ = "parquet"; }
    void SetJsonPath(const std::string &path) { file_path_ = path; file_format_ = "json"; }
    void SetAvroPath(const std::string &path) { file_path_ = path; file_format_ = "avro"; }
    void SetArrowPath(const std::string &path) { file_path_ = path; file_format_ = "arrow"; }
    const std::string &GetFilePath() const { return file_path_; }
    char GetFileDelimiter() const { return file_delimiter_; }
    const std::string &GetFileFormat() const { return file_format_; }

    // Optional cached ParquetReader shared across queries — the schema-
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
    std::string view_query_;
    std::string file_path_;
    char file_delimiter_ = ',';
    std::string file_format_ = "csv";
    std::shared_ptr<ParquetReader> cached_parquet_reader_;
};

} // namespace slothdb

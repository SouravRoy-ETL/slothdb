#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace slothdb {

// SQLite database file scanner.
// Reads SQLite 3 database files directly without linking to libsqlite3.
// This is a simplified reader that handles basic table scans for simple types.
//
// SQLite file format:
//   - 100-byte header
//   - Pages of fixed size (typically 4096 bytes)
//   - sqlite_master table on page 1 (stores schema)
//   - B-tree pages for table and index data
//
// This reader handles leaf table B-tree pages with basic record format.

class SQLiteScanner {
public:
    explicit SQLiteScanner(const std::string &path);

    // List all table names.
    std::vector<std::string> ListTables();

    // Get column info for a table.
    struct ColumnInfo {
        std::string name;
        LogicalType type;
    };
    std::vector<ColumnInfo> GetColumns(const std::string &table_name);

    // Scan all rows from a table.
    std::vector<std::vector<Value>> ScanTable(const std::string &table_name);

private:
    void ReadHeader();
    void ReadMasterTable();

    // Read a varint from the file data.
    int64_t ReadVarint(const uint8_t *data, size_t &pos);

    // Read a record from a leaf cell.
    std::vector<Value> ReadRecord(const uint8_t *data, size_t pos, size_t len);

    // Scan a B-tree page for leaf records.
    std::vector<std::vector<Value>> ScanBTreePage(uint32_t page_num);

    std::string path_;
    std::vector<uint8_t> file_data_;
    uint32_t page_size_ = 4096;
    uint32_t num_pages_ = 0;

    struct TableMeta {
        std::string name;
        uint32_t root_page = 0;
        std::string create_sql;
    };
    std::vector<TableMeta> tables_;
    bool header_read_ = false;
};

} // namespace slothdb

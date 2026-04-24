#include "slothdb/storage/sqlite_scanner.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include <cstring>
#include <algorithm>
#include <sstream>

namespace slothdb {

SQLiteScanner::SQLiteScanner(const std::string &path) : path_(path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open SQLite file: " + path);

    auto size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    file_data_.resize(size);
    file.read(reinterpret_cast<char *>(file_data_.data()), size);

    ReadHeader();
    ReadMasterTable();
}

void SQLiteScanner::ReadHeader() {
    if (file_data_.size() < 100)
        throw IOException(ErrorCode::CORRUPT_DATA, "File too small for SQLite");

    // Check magic string "SQLite format 3\000".
    if (std::memcmp(file_data_.data(), "SQLite format 3\000", 16) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not a SQLite database");

    // Page size (bytes 16-17, big-endian).
    page_size_ = (static_cast<uint32_t>(file_data_[16]) << 8) | file_data_[17];
    if (page_size_ == 1) page_size_ = 65536; // Special case in SQLite spec.

    num_pages_ = static_cast<uint32_t>(file_data_.size() / page_size_);
    header_read_ = true;
}

int64_t SQLiteScanner::ReadVarint(const uint8_t *data, size_t &pos) {
    int64_t result = 0;
    for (int i = 0; i < 9; i++) {
        uint8_t b = data[pos++];
        if (i < 8) {
            result = (result << 7) | (b & 0x7F);
            if ((b & 0x80) == 0) return result;
        } else {
            result = (result << 8) | b;
        }
    }
    return result;
}

std::vector<Value> SQLiteScanner::ReadRecord(const uint8_t *data, size_t pos, size_t len) {
    std::vector<Value> values;
    size_t end = pos + len;

    // Header: varint header_size, then serial type varints.
    size_t header_start = pos;
    auto header_size = ReadVarint(data, pos);
    size_t header_end = header_start + static_cast<size_t>(header_size);

    std::vector<int64_t> serial_types;
    while (pos < header_end && pos < end) {
        serial_types.push_back(ReadVarint(data, pos));
    }

    // Data follows header.
    pos = header_end;
    for (auto serial_type : serial_types) {
        if (pos >= end) { values.push_back(Value()); continue; }

        if (serial_type == 0) {
            values.push_back(Value()); // NULL
        } else if (serial_type == 1) {
            // 8-bit int.
            int8_t val = static_cast<int8_t>(data[pos++]);
            values.push_back(Value::INTEGER(val));
        } else if (serial_type == 2) {
            // 16-bit big-endian int.
            int16_t val = static_cast<int16_t>((data[pos] << 8) | data[pos + 1]);
            pos += 2;
            values.push_back(Value::INTEGER(val));
        } else if (serial_type == 3) {
            // 24-bit big-endian int.
            int32_t val = (data[pos] << 16) | (data[pos + 1] << 8) | data[pos + 2];
            if (val & 0x800000) val |= static_cast<int32_t>(0xFF000000);
            pos += 3;
            values.push_back(Value::INTEGER(val));
        } else if (serial_type == 4) {
            // 32-bit big-endian int.
            int32_t val;
            uint8_t buf[4] = {data[pos + 3], data[pos + 2], data[pos + 1], data[pos]};
            std::memcpy(&val, buf, 4);
            pos += 4;
            values.push_back(Value::INTEGER(val));
        } else if (serial_type == 5) {
            // 48-bit big-endian int.
            int64_t val = 0;
            for (int i = 0; i < 6; i++) val = (val << 8) | data[pos++];
            if (val & 0x800000000000LL) val |= static_cast<int64_t>(0xFFFF000000000000LL);
            values.push_back(Value::BIGINT(val));
        } else if (serial_type == 6) {
            // 64-bit big-endian int.
            int64_t val = 0;
            for (int i = 0; i < 8; i++) val = (val << 8) | data[pos++];
            values.push_back(Value::BIGINT(val));
        } else if (serial_type == 7) {
            // 64-bit IEEE float (big-endian).
            uint8_t buf[8];
            for (int i = 0; i < 8; i++) buf[7 - i] = data[pos++];
            double val;
            std::memcpy(&val, buf, 8);
            values.push_back(Value::DOUBLE(val));
        } else if (serial_type == 8) {
            values.push_back(Value::INTEGER(0)); // Constant 0.
        } else if (serial_type == 9) {
            values.push_back(Value::INTEGER(1)); // Constant 1.
        } else if (serial_type >= 12 && (serial_type % 2) == 0) {
            // BLOB.
            size_t blob_len = static_cast<size_t>((serial_type - 12) / 2);
            pos += blob_len; // Skip blob data.
            values.push_back(Value::VARCHAR("[BLOB]"));
        } else if (serial_type >= 13 && (serial_type % 2) == 1) {
            // Text string.
            size_t str_len = static_cast<size_t>((serial_type - 13) / 2);
            if (pos + str_len <= end) {
                values.push_back(Value::VARCHAR(
                    std::string(reinterpret_cast<const char *>(&data[pos]), str_len)));
            } else {
                values.push_back(Value());
            }
            pos += str_len;
        } else {
            values.push_back(Value());
        }
    }

    return values;
}

std::vector<std::vector<Value>> SQLiteScanner::ScanBTreePage(uint32_t page_num) {
    std::vector<std::vector<Value>> results;
    if (page_num == 0 || page_num > num_pages_) return results;

    size_t page_offset = static_cast<size_t>(page_num - 1) * page_size_;
    size_t header_offset = (page_num == 1) ? 100 : 0; // Page 1 has 100-byte file header.
    auto *page = &file_data_[page_offset];
    auto *header = &page[header_offset];

    uint8_t page_type = header[0];
    // 13 = leaf table, 5 = interior table, 10 = leaf index, 2 = interior index.

    if (page_type == 13) {
        // Leaf table B-tree page.
        uint16_t num_cells = (header[3] << 8) | header[4];
        size_t cell_ptr_offset = header_offset + 8;

        for (uint16_t i = 0; i < num_cells; i++) {
            uint16_t cell_offset = (page[cell_ptr_offset + i * 2] << 8) |
                                    page[cell_ptr_offset + i * 2 + 1];
            size_t pos = cell_offset;

            // Cell format: payload_size(varint), rowid(varint), payload
            auto payload_size = ReadVarint(page, pos);
            auto rowid = ReadVarint(page, pos);
            (void)rowid;

            auto record = ReadRecord(page, pos, static_cast<size_t>(payload_size));
            results.push_back(std::move(record));
        }
    } else if (page_type == 5) {
        // Interior table B-tree page - recurse into children.
        uint16_t num_cells = (header[3] << 8) | header[4];
        uint32_t right_child = (header[8] << 24) | (header[9] << 16) |
                                (header[10] << 8) | header[11];
        size_t cell_ptr_offset = header_offset + 12;

        for (uint16_t i = 0; i < num_cells; i++) {
            uint16_t cell_offset = (page[cell_ptr_offset + i * 2] << 8) |
                                    page[cell_ptr_offset + i * 2 + 1];
            // Interior cell: left_child(4), rowid(varint)
            uint32_t left_child = (page[cell_offset] << 24) | (page[cell_offset + 1] << 16) |
                                   (page[cell_offset + 2] << 8) | page[cell_offset + 3];
            auto children = ScanBTreePage(left_child);
            results.insert(results.end(), children.begin(), children.end());
        }
        // Right-most child.
        auto right_children = ScanBTreePage(right_child);
        results.insert(results.end(), right_children.begin(), right_children.end());
    }

    return results;
}

void SQLiteScanner::ReadMasterTable() {
    // sqlite_master is always on page 1.
    auto rows = ScanBTreePage(1);
    for (auto &row : rows) {
        if (row.size() >= 5) {
            auto type = row[0].ToString();
            auto name = row[1].ToString();
            auto root_page_str = row[3].ToString();
            auto sql = row[4].ToString();

            if (type == "table" && name != "sqlite_sequence") {
                TableMeta meta;
                meta.name = name;
                try { meta.root_page = static_cast<uint32_t>(std::stoul(root_page_str)); }
                catch (...) { continue; }
                meta.create_sql = sql;
                tables_.push_back(std::move(meta));
            }
        }
    }
}

std::vector<std::string> SQLiteScanner::ListTables() {
    std::vector<std::string> names;
    for (auto &t : tables_) names.push_back(t.name);
    return names;
}

std::vector<SQLiteScanner::ColumnInfo>
SQLiteScanner::GetColumns(const std::string &table_name) {
    std::vector<ColumnInfo> columns;
    for (auto &t : tables_) {
        if (t.name == table_name) {
            // Parse CREATE TABLE statement to extract columns.
            auto &sql = t.create_sql;
            auto paren = sql.find('(');
            if (paren == std::string::npos) break;
            auto close_paren = sql.rfind(')');
            if (close_paren == std::string::npos) break;

            auto cols_str = sql.substr(paren + 1, close_paren - paren - 1);
            // Split by comma.
            std::istringstream stream(cols_str);
            std::string col_def;
            while (std::getline(stream, col_def, ',')) {
                // Trim.
                auto start = col_def.find_first_not_of(" \t\n\r");
                if (start == std::string::npos) continue;
                col_def = col_def.substr(start);

                // Extract name and type.
                auto space = col_def.find(' ');
                ColumnInfo info;
                info.name = col_def.substr(0, space);

                std::string type_str = "TEXT";
                if (space != std::string::npos) {
                    auto type_start = col_def.find_first_not_of(" \t", space + 1);
                    if (type_start != std::string::npos) {
                        auto type_end = col_def.find_first_of(" \t(,)", type_start);
                        type_str = StringUtil::Upper(col_def.substr(type_start,
                            type_end != std::string::npos ? type_end - type_start : std::string::npos));
                    }
                }

                if (type_str == "INTEGER" || type_str == "INT")
                    info.type = LogicalType::INTEGER();
                else if (type_str == "REAL" || type_str == "FLOAT" || type_str == "DOUBLE")
                    info.type = LogicalType::DOUBLE();
                else
                    info.type = LogicalType::VARCHAR();

                columns.push_back(std::move(info));
            }
            break;
        }
    }
    return columns;
}

std::vector<std::vector<Value>>
SQLiteScanner::ScanTable(const std::string &table_name) {
    for (auto &t : tables_) {
        if (t.name == table_name) {
            return ScanBTreePage(t.root_page);
        }
    }
    throw CatalogException("Table '" + table_name + "' not found in SQLite database");
}

// Stream the rows of `table_name` into typed DataChunk vectors. Reuses the
// existing B-tree scan (which returns Value rows) and copies the results
// into typed slots chunk-by-chunk, batching at VECTOR_SIZE. Same correctness
// as ScanTable + BulkLoadRows, minus the DataTable roundtrip.
void SQLiteScanner::ScanTableIntoChunks(std::vector<DataChunk> &chunks,
                                         const std::vector<LogicalType> &types,
                                         const std::string &table_name) {
    auto rows = ScanTable(table_name);
    if (rows.empty()) return;

    DataChunk cur;
    cur.Initialize(types);
    idx_t cur_count = 0;
    auto num_cols = static_cast<idx_t>(types.size());

    struct ColPtr {
        LogicalTypeId tid;
        data_ptr_t data;
        ValidityMask *validity;
        VectorStringBuffer *str_buf;
    };
    std::vector<ColPtr> cp(num_cols);
    auto refresh_cp = [&]() {
        for (idx_t c = 0; c < num_cols; c++) {
            auto &v = cur.GetVector(c);
            cp[c].tid = types[c].id();
            cp[c].data = v.GetData();
            cp[c].validity = &v.GetValidity();
            cp[c].str_buf = (cp[c].tid == LogicalTypeId::VARCHAR)
                                ? &v.GetStringBuffer() : nullptr;
        }
    };
    refresh_cp();

    auto flush = [&]() {
        if (cur_count == 0) return;
        cur.SetCardinality(cur_count);
        chunks.push_back(std::move(cur));
        cur.Initialize(types);
        cur_count = 0;
        refresh_cp();
    };

    for (auto &row : rows) {
        for (idx_t c = 0; c < num_cols && c < row.size(); c++) {
            auto &col = cp[c];
            const auto &v = row[c];
            if (v.IsNull()) {
                col.validity->SetInvalid(cur_count);
                continue;
            }
            try {
                switch (col.tid) {
                case LogicalTypeId::BOOLEAN: {
                    auto *arr = reinterpret_cast<bool *>(col.data);
                    arr[cur_count] = v.GetValue<bool>();
                    break;
                }
                case LogicalTypeId::INTEGER: {
                    auto *arr = reinterpret_cast<int32_t *>(col.data);
                    arr[cur_count] = v.GetValue<int32_t>();
                    break;
                }
                case LogicalTypeId::BIGINT: {
                    auto *arr = reinterpret_cast<int64_t *>(col.data);
                    arr[cur_count] = v.GetValue<int64_t>();
                    break;
                }
                case LogicalTypeId::FLOAT: {
                    auto *arr = reinterpret_cast<float *>(col.data);
                    arr[cur_count] = static_cast<float>(v.GetValue<double>());
                    break;
                }
                case LogicalTypeId::DOUBLE: {
                    auto *arr = reinterpret_cast<double *>(col.data);
                    arr[cur_count] = v.GetValue<double>();
                    break;
                }
                case LogicalTypeId::VARCHAR: {
                    auto s = v.GetValue<std::string>();
                    auto *arr = reinterpret_cast<string_t *>(col.data);
                    auto len = static_cast<uint32_t>(s.size());
                    if (s.size() <= string_t::INLINE_LENGTH) {
                        arr[cur_count] = string_t(s.data(), len);
                    } else {
                        const char *heap = col.str_buf->AddString(s.data(), s.size());
                        arr[cur_count] = string_t(heap, len);
                    }
                    break;
                }
                default:
                    col.validity->SetInvalid(cur_count);
                    break;
                }
            } catch (...) {
                col.validity->SetInvalid(cur_count);
            }
        }
        // Fill any remaining columns with NULL.
        for (idx_t c = row.size(); c < num_cols; c++) {
            cp[c].validity->SetInvalid(cur_count);
        }
        cur_count++;
        if (cur_count >= VECTOR_SIZE) flush();
    }
    flush();
}

} // namespace slothdb

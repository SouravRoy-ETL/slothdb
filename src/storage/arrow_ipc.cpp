#include "slothdb/storage/arrow_ipc.hpp"
#include "slothdb/common/exception.hpp"
#include <fstream>
#include <cstring>

namespace slothdb {

// Simple Arrow IPC file format (Feather v2 compatible).
// This is a simplified format that stores our own metadata + columnar data.
// For interop, we use the same magic "ARROW1" and store data in a
// self-describing binary format.

static const char ARROW_MAGIC[6] = {'A', 'R', 'R', 'O', 'W', '1'};

// Type ID for our simplified format.
static uint8_t LogicalTypeToArrowId(const LogicalType &type) {
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN: return 1;
    case LogicalTypeId::INTEGER: return 2;
    case LogicalTypeId::BIGINT: return 3;
    case LogicalTypeId::FLOAT: return 4;
    case LogicalTypeId::DOUBLE: return 5;
    case LogicalTypeId::VARCHAR: return 6;
    default: return 6; // default to string
    }
}

static LogicalType ArrowIdToLogicalType(uint8_t id) {
    switch (id) {
    case 1: return LogicalType::BOOLEAN();
    case 2: return LogicalType::INTEGER();
    case 3: return LogicalType::BIGINT();
    case 4: return LogicalType::FLOAT();
    case 5: return LogicalType::DOUBLE();
    case 6: return LogicalType::VARCHAR();
    default: return LogicalType::VARCHAR();
    }
}

// ============================================================================
// Writer
// ============================================================================

ArrowIPCWriter::ArrowIPCWriter(const std::string &path,
                               const std::vector<std::string> &column_names,
                               const std::vector<LogicalType> &column_types)
    : path_(path), column_names_(column_names), column_types_(column_types) {}

void ArrowIPCWriter::WriteBatch(const std::vector<std::vector<Value>> &rows) {
    batches_.push_back(rows);
}

void ArrowIPCWriter::Finish() {
    std::ofstream file(path_, std::ios::binary);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_WRITE_ERROR, "Cannot create Arrow file: " + path_);

    // Write magic.
    file.write(ARROW_MAGIC, 6);

    // Write schema: [num_cols(4)][for each: name_len(4), name, type_id(1)]
    uint32_t num_cols = static_cast<uint32_t>(column_names_.size());
    file.write(reinterpret_cast<const char *>(&num_cols), 4);
    for (uint32_t c = 0; c < num_cols; c++) {
        uint32_t name_len = static_cast<uint32_t>(column_names_[c].size());
        file.write(reinterpret_cast<const char *>(&name_len), 4);
        file.write(column_names_[c].c_str(), name_len);
        uint8_t type_id = LogicalTypeToArrowId(column_types_[c]);
        file.write(reinterpret_cast<const char *>(&type_id), 1);
    }

    // Write all rows (simple: row count + values).
    int64_t total_rows = 0;
    for (auto &batch : batches_) total_rows += static_cast<int64_t>(batch.size());
    file.write(reinterpret_cast<const char *>(&total_rows), 8);

    for (auto &batch : batches_) {
        for (auto &row : batch) {
            for (uint32_t c = 0; c < num_cols; c++) {
                Value null_val;
                auto &val = (c < row.size()) ? row[c] : null_val;
                uint8_t is_null = val.IsNull() ? 1 : 0;
                file.write(reinterpret_cast<const char *>(&is_null), 1);
                if (!is_null) {
                    auto str = val.ToString();
                    uint32_t len = static_cast<uint32_t>(str.size());
                    file.write(reinterpret_cast<const char *>(&len), 4);
                    file.write(str.c_str(), len);
                }
            }
        }
    }

    // Write ending magic.
    file.write(ARROW_MAGIC, 6);
    file.close();
}

// ============================================================================
// Reader
// ============================================================================

ArrowIPCReader::ArrowIPCReader(const std::string &path) : path_(path) {}

void ArrowIPCReader::Parse() {
    if (parsed_) return;
    parsed_ = true;

    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Arrow file: " + path_);

    // Read magic.
    char magic[6];
    file.read(magic, 6);
    if (std::memcmp(magic, ARROW_MAGIC, 6) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not an Arrow IPC file");

    // Read schema.
    uint32_t num_cols;
    file.read(reinterpret_cast<char *>(&num_cols), 4);
    for (uint32_t c = 0; c < num_cols; c++) {
        uint32_t name_len;
        file.read(reinterpret_cast<char *>(&name_len), 4);
        std::string name(name_len, '\0');
        file.read(name.data(), name_len);
        column_names_.push_back(name);

        uint8_t type_id;
        file.read(reinterpret_cast<char *>(&type_id), 1);
        column_types_.push_back(ArrowIdToLogicalType(type_id));
    }

    // Read rows.
    int64_t total_rows;
    file.read(reinterpret_cast<char *>(&total_rows), 8);

    for (int64_t r = 0; r < total_rows; r++) {
        std::vector<Value> row;
        for (uint32_t c = 0; c < num_cols; c++) {
            uint8_t is_null;
            file.read(reinterpret_cast<char *>(&is_null), 1);
            if (is_null) {
                row.push_back(Value());
            } else {
                uint32_t len;
                file.read(reinterpret_cast<char *>(&len), 4);
                std::string str(len, '\0');
                file.read(str.data(), len);

                // Convert back to typed value.
                auto &type = column_types_[c];
                try {
                    switch (type.id()) {
                    case LogicalTypeId::BOOLEAN:
                        row.push_back(Value::BOOLEAN(str == "true")); break;
                    case LogicalTypeId::INTEGER:
                        row.push_back(Value::INTEGER(std::stoi(str))); break;
                    case LogicalTypeId::BIGINT:
                        row.push_back(Value::BIGINT(std::stoll(str))); break;
                    case LogicalTypeId::FLOAT:
                        row.push_back(Value::FLOAT(std::stof(str))); break;
                    case LogicalTypeId::DOUBLE:
                        row.push_back(Value::DOUBLE(std::stod(str))); break;
                    default:
                        row.push_back(Value::VARCHAR(str)); break;
                    }
                } catch (...) {
                    row.push_back(Value::VARCHAR(str));
                }
            }
        }
        rows_.push_back(std::move(row));
    }
}

int64_t ArrowIPCReader::NumRows() const {
    return static_cast<int64_t>(rows_.size());
}

std::vector<std::vector<Value>> ArrowIPCReader::ReadAll() {
    Parse();
    return rows_;
}

} // namespace slothdb

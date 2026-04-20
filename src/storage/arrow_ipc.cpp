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

    // Read schema with validation.
    uint32_t num_cols;
    file.read(reinterpret_cast<char *>(&num_cols), 4);
    if (!file.good() || num_cols > 10000)
        throw IOException(ErrorCode::CORRUPT_DATA, "Invalid Arrow column count");
    for (uint32_t c = 0; c < num_cols; c++) {
        uint32_t name_len;
        file.read(reinterpret_cast<char *>(&name_len), 4);
        if (!file.good() || name_len > 1000000)
            throw IOException(ErrorCode::CORRUPT_DATA, "Invalid Arrow column name length");
        std::string name(name_len, '\0');
        file.read(name.data(), name_len);
        if (!file.good())
            throw IOException(ErrorCode::CORRUPT_DATA, "Truncated Arrow file");
        column_names_.push_back(name);

        uint8_t type_id;
        file.read(reinterpret_cast<char *>(&type_id), 1);
        column_types_.push_back(ArrowIdToLogicalType(type_id));
    }

    // Read rows.
    int64_t total_rows;
    file.read(reinterpret_cast<char *>(&total_rows), 8);
    if (!file.good() || total_rows < 0 || total_rows > 1000000000LL)
        throw IOException(ErrorCode::CORRUPT_DATA, "Invalid Arrow row count");

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

// Parse only the schema header — magic bytes + column list — so callers
// can register the catalog entry without reading every row.
void ArrowIPCReader::DetectSchemaLight() {
    if (schema_parsed_) return;
    schema_parsed_ = true;

    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Arrow file: " + path_);

    char magic[6];
    file.read(magic, 6);
    if (std::memcmp(magic, ARROW_MAGIC, 6) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not an Arrow IPC file");

    uint32_t num_cols;
    file.read(reinterpret_cast<char *>(&num_cols), 4);
    if (!file.good() || num_cols > 10000)
        throw IOException(ErrorCode::CORRUPT_DATA, "Invalid Arrow column count");

    for (uint32_t c = 0; c < num_cols; c++) {
        uint32_t name_len;
        file.read(reinterpret_cast<char *>(&name_len), 4);
        if (!file.good() || name_len > 1000000)
            throw IOException(ErrorCode::CORRUPT_DATA, "Invalid Arrow column name length");
        std::string name(name_len, '\0');
        file.read(name.data(), name_len);
        if (!file.good())
            throw IOException(ErrorCode::CORRUPT_DATA, "Truncated Arrow file");
        column_names_.push_back(name);

        uint8_t type_id;
        file.read(reinterpret_cast<char *>(&type_id), 1);
        column_types_.push_back(ArrowIdToLogicalType(type_id));
    }
}

// Stream-parse the Arrow data directly into typed DataChunk Vectors.
// Each chunk holds up to VECTOR_SIZE rows. No Value-boxed rows_ stored.
void ArrowIPCReader::ReadIntoChunks(std::vector<DataChunk> &chunks,
                                     const std::vector<LogicalType> &types) {
    if (!schema_parsed_) DetectSchemaLight();

    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Arrow file: " + path_);

    // Skip magic (6) + num_cols (4).
    file.seekg(10, std::ios::beg);
    uint32_t num_cols = static_cast<uint32_t>(column_names_.size());
    // Skip each column's {name_len, name, type_id} from the schema.
    for (uint32_t c = 0; c < num_cols; c++) {
        uint32_t name_len;
        file.read(reinterpret_cast<char *>(&name_len), 4);
        file.seekg(name_len + 1, std::ios::cur);
    }

    // Now we're positioned at the row count.
    int64_t total_rows;
    file.read(reinterpret_cast<char *>(&total_rows), 8);
    if (!file.good() || total_rows < 0 || total_rows > 1000000000LL)
        throw IOException(ErrorCode::CORRUPT_DATA, "Invalid Arrow row count");

    if (total_rows == 0) return;

    DataChunk current;
    current.Initialize(types);
    idx_t cur_count = 0;
    auto num_cols_out = static_cast<idx_t>(types.size());

    struct ColPtr {
        LogicalTypeId tid;
        data_ptr_t data;
        ValidityMask *validity;
        VectorStringBuffer *str_buf;
    };
    std::vector<ColPtr> cp(num_cols_out);
    auto refresh_cp = [&]() {
        for (idx_t c = 0; c < num_cols_out; c++) {
            auto &v = current.GetVector(c);
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
        current.SetCardinality(cur_count);
        chunks.push_back(std::move(current));
        current.Initialize(types);
        cur_count = 0;
        refresh_cp();
    };

    for (int64_t r = 0; r < total_rows; r++) {
        for (uint32_t c = 0; c < num_cols; c++) {
            uint8_t is_null;
            file.read(reinterpret_cast<char *>(&is_null), 1);

            if (c >= num_cols_out) {
                // Skip any columns beyond the output schema.
                if (!is_null) {
                    uint32_t len; file.read(reinterpret_cast<char *>(&len), 4);
                    file.seekg(len, std::ios::cur);
                }
                continue;
            }

            auto &col = cp[c];
            if (is_null) {
                col.validity->SetInvalid(cur_count);
                continue;
            }

            uint32_t len;
            file.read(reinterpret_cast<char *>(&len), 4);
            std::string s(len, '\0');
            file.read(s.data(), len);

            try {
                switch (col.tid) {
                case LogicalTypeId::BOOLEAN: {
                    auto *arr = reinterpret_cast<bool *>(col.data);
                    arr[cur_count] = (s == "true" || s == "1");
                    break;
                }
                case LogicalTypeId::INTEGER: {
                    auto *arr = reinterpret_cast<int32_t *>(col.data);
                    arr[cur_count] = std::stoi(s);
                    break;
                }
                case LogicalTypeId::BIGINT: {
                    auto *arr = reinterpret_cast<int64_t *>(col.data);
                    arr[cur_count] = std::stoll(s);
                    break;
                }
                case LogicalTypeId::FLOAT: {
                    auto *arr = reinterpret_cast<float *>(col.data);
                    arr[cur_count] = std::stof(s);
                    break;
                }
                case LogicalTypeId::DOUBLE: {
                    auto *arr = reinterpret_cast<double *>(col.data);
                    arr[cur_count] = std::stod(s);
                    break;
                }
                case LogicalTypeId::VARCHAR: {
                    auto *arr = reinterpret_cast<string_t *>(col.data);
                    if (len <= string_t::INLINE_LENGTH) {
                        arr[cur_count] = string_t(s.data(), static_cast<uint32_t>(len));
                    } else {
                        const char *heap = col.str_buf->AddString(s.data(), len);
                        arr[cur_count] = string_t(heap, static_cast<uint32_t>(len));
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

        cur_count++;
        if (cur_count >= VECTOR_SIZE) flush();
    }
    flush();
}

} // namespace slothdb

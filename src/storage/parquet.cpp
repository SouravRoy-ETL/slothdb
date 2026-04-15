#include "slothdb/storage/parquet.hpp"
#include "slothdb/common/exception.hpp"
#include <cstring>
#include <algorithm>

namespace slothdb {

static const char PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};

static ParquetType LogicalToParquetType(const LogicalType &type) {
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN: return ParquetType::BOOLEAN;
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE: return ParquetType::INT32;
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP: return ParquetType::INT64;
    case LogicalTypeId::FLOAT: return ParquetType::FLOAT;
    case LogicalTypeId::DOUBLE: return ParquetType::DOUBLE;
    default: return ParquetType::BYTE_ARRAY;
    }
}

static LogicalType ParquetToLogicalType(ParquetType type) {
    switch (type) {
    case ParquetType::BOOLEAN: return LogicalType::BOOLEAN();
    case ParquetType::INT32: return LogicalType::INTEGER();
    case ParquetType::INT64: return LogicalType::BIGINT();
    case ParquetType::FLOAT: return LogicalType::FLOAT();
    case ParquetType::DOUBLE: return LogicalType::DOUBLE();
    case ParquetType::BYTE_ARRAY: return LogicalType::VARCHAR();
    }
    return LogicalType::VARCHAR();
}

// ============================================================================
// Thrift Compact Protocol Helpers
// ============================================================================

void ParquetWriter::WriteVarInt(std::vector<uint8_t> &buf, uint64_t val) {
    while (val >= 0x80) {
        buf.push_back(static_cast<uint8_t>(val | 0x80));
        val >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(val));
}

void ParquetWriter::WriteThriftFieldI32(std::vector<uint8_t> &buf, int field_id,
                                         int32_t val) {
    // Compact: field header + zigzag-encoded i32.
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 5)); // type 5 = i32
    uint32_t zigzag = static_cast<uint32_t>((val << 1) ^ (val >> 31));
    WriteVarInt(buf, zigzag);
}

void ParquetWriter::WriteThriftFieldI64(std::vector<uint8_t> &buf, int field_id,
                                         int64_t val) {
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 6)); // type 6 = i64
    uint64_t zigzag = static_cast<uint64_t>((val << 1) ^ (val >> 63));
    WriteVarInt(buf, zigzag);
}

void ParquetWriter::WriteThriftFieldString(std::vector<uint8_t> &buf, int field_id,
                                            const std::string &val) {
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 8)); // type 8 = binary
    WriteVarInt(buf, val.size());
    buf.insert(buf.end(), val.begin(), val.end());
}

void ParquetWriter::WriteThriftFieldList(std::vector<uint8_t> &buf, int field_id,
                                          int elem_type, int count) {
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 12)); // type 12 = list
    if (count < 15) {
        buf.push_back(static_cast<uint8_t>((count << 4) | elem_type));
    } else {
        buf.push_back(static_cast<uint8_t>(0xF0 | elem_type));
        WriteVarInt(buf, count);
    }
}

void ParquetWriter::WriteThriftFieldStruct(std::vector<uint8_t> &buf, int field_id) {
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 12)); // type 12 = struct
}

void ParquetWriter::WriteThriftStop(std::vector<uint8_t> &buf) {
    buf.push_back(0); // stop field
}

// ============================================================================
// Parquet Writer
// ============================================================================

ParquetWriter::ParquetWriter(const std::string &path,
                             const std::vector<std::string> &column_names,
                             const std::vector<LogicalType> &column_types)
    : path_(path), column_names_(column_names), column_types_(column_types) {
    file_.open(path, std::ios::binary);
    if (!file_.is_open())
        throw IOException(ErrorCode::FILE_WRITE_ERROR, "Cannot create Parquet file: " + path);

    // Write magic.
    file_.write(PARQUET_MAGIC, 4);
    meta_.column_names = column_names;
    meta_.column_types = column_types;
}

ParquetWriter::~ParquetWriter() {
    if (!finished_) Finish();
}

void ParquetWriter::WriteColumnChunk(const std::vector<Value> &values,
                                      const LogicalType &type,
                                      ParquetColumnMeta &meta) {
    auto ptype = LogicalToParquetType(type);
    meta.parquet_type = ptype;
    meta.slothdb_type = type;
    meta.num_values = static_cast<int64_t>(values.size());
    meta.data_offset = current_offset_;

    // Write PLAIN-encoded data.
    // Format: [page_header][data]
    // Simplified: we write raw values without a proper page header.
    // Each value: [4-byte is_null flag][data]
    std::vector<uint8_t> data;

    Value min_val, max_val;
    bool has_min = false;

    for (auto &v : values) {
        if (v.IsNull()) {
            uint32_t null_marker = 0xFFFFFFFF;
            auto *p = reinterpret_cast<uint8_t *>(&null_marker);
            data.insert(data.end(), p, p + 4);
            continue;
        }

        uint32_t not_null = 0;
        auto *np = reinterpret_cast<uint8_t *>(&not_null);
        data.insert(data.end(), np, np + 4);

        // Track stats.
        if (!has_min || v < min_val) min_val = v;
        if (!has_min || max_val < v) max_val = v;
        has_min = true;

        switch (ptype) {
        case ParquetType::BOOLEAN: {
            uint8_t b = v.GetValue<bool>() ? 1 : 0;
            data.push_back(b);
            break;
        }
        case ParquetType::INT32: {
            int32_t val = v.GetValue<int32_t>();
            auto *p = reinterpret_cast<uint8_t *>(&val);
            data.insert(data.end(), p, p + 4);
            break;
        }
        case ParquetType::INT64: {
            int64_t val = (v.type().id() == LogicalTypeId::INTEGER)
                ? static_cast<int64_t>(v.GetValue<int32_t>()) : v.GetValue<int64_t>();
            auto *p = reinterpret_cast<uint8_t *>(&val);
            data.insert(data.end(), p, p + 8);
            break;
        }
        case ParquetType::FLOAT: {
            float val = v.GetValue<float>();
            auto *p = reinterpret_cast<uint8_t *>(&val);
            data.insert(data.end(), p, p + 4);
            break;
        }
        case ParquetType::DOUBLE: {
            double val = (v.type().id() == LogicalTypeId::INTEGER)
                ? static_cast<double>(v.GetValue<int32_t>())
                : (v.type().id() == LogicalTypeId::FLOAT)
                    ? static_cast<double>(v.GetValue<float>())
                    : v.GetValue<double>();
            auto *p = reinterpret_cast<uint8_t *>(&val);
            data.insert(data.end(), p, p + 8);
            break;
        }
        case ParquetType::BYTE_ARRAY: {
            auto s = v.ToString();
            uint32_t len = static_cast<uint32_t>(s.size());
            auto *lp = reinterpret_cast<uint8_t *>(&len);
            data.insert(data.end(), lp, lp + 4);
            data.insert(data.end(), s.begin(), s.end());
            break;
        }
        }
    }

    file_.write(reinterpret_cast<const char *>(data.data()), data.size());
    meta.data_size = static_cast<int64_t>(data.size());
    meta.has_stats = has_min;
    meta.min_value = min_val;
    meta.max_value = max_val;
    current_offset_ += meta.data_size;
}

void ParquetWriter::WriteRowGroup(const std::vector<std::vector<Value>> &rows) {
    if (rows.empty()) return;

    ParquetRowGroup rg;
    rg.num_rows = static_cast<int64_t>(rows.size());

    // Transpose rows to columns.
    idx_t num_cols = column_types_.size();
    for (idx_t col = 0; col < num_cols; col++) {
        std::vector<Value> column_data;
        for (auto &row : rows) {
            column_data.push_back(col < row.size() ? row[col] : Value());
        }

        ParquetColumnMeta col_meta;
        col_meta.name = column_names_[col];
        WriteColumnChunk(column_data, column_types_[col], col_meta);
        rg.columns.push_back(std::move(col_meta));
    }

    meta_.num_rows += rg.num_rows;
    meta_.row_groups.push_back(std::move(rg));
}

void ParquetWriter::Finish() {
    if (finished_) return;
    finished_ = true;

    // Write footer: simplified metadata as binary.
    // Format: [num_row_groups(4)][for each rg: num_rows(8), num_cols(4),
    //          for each col: name_len(4), name, type(4), offset(8), size(8),
    //          has_stats(1), min_str_len(4), min_str, max_str_len(4), max_str]
    // Then: [footer_size(4)][PAR1]

    (void)current_offset_;
    std::vector<uint8_t> footer;

    uint32_t num_rg = static_cast<uint32_t>(meta_.row_groups.size());
    auto *rp = reinterpret_cast<uint8_t *>(&num_rg);
    footer.insert(footer.end(), rp, rp + 4);

    // Total rows.
    auto *trp = reinterpret_cast<uint8_t *>(&meta_.num_rows);
    footer.insert(footer.end(), trp, trp + 8);

    // Number of columns.
    uint32_t num_cols = static_cast<uint32_t>(column_names_.size());
    auto *ncp = reinterpret_cast<uint8_t *>(&num_cols);
    footer.insert(footer.end(), ncp, ncp + 4);

    // Column names and types.
    for (idx_t c = 0; c < num_cols; c++) {
        uint32_t name_len = static_cast<uint32_t>(column_names_[c].size());
        auto *nlp = reinterpret_cast<uint8_t *>(&name_len);
        footer.insert(footer.end(), nlp, nlp + 4);
        footer.insert(footer.end(), column_names_[c].begin(), column_names_[c].end());

        int32_t type_id = static_cast<int32_t>(LogicalToParquetType(column_types_[c]));
        auto *tip = reinterpret_cast<uint8_t *>(&type_id);
        footer.insert(footer.end(), tip, tip + 4);
    }

    // Row group metadata.
    for (auto &rg : meta_.row_groups) {
        auto *nrp = reinterpret_cast<uint8_t *>(&rg.num_rows);
        footer.insert(footer.end(), nrp, nrp + 8);

        for (auto &col : rg.columns) {
            auto *dop = reinterpret_cast<uint8_t *>(&col.data_offset);
            footer.insert(footer.end(), dop, dop + 8);
            auto *dsp = reinterpret_cast<uint8_t *>(&col.data_size);
            footer.insert(footer.end(), dsp, dsp + 8);

            footer.push_back(col.has_stats ? 1 : 0);
            if (col.has_stats) {
                auto min_s = col.min_value.ToString();
                auto max_s = col.max_value.ToString();
                uint32_t min_len = static_cast<uint32_t>(min_s.size());
                uint32_t max_len = static_cast<uint32_t>(max_s.size());
                auto *mlp = reinterpret_cast<uint8_t *>(&min_len);
                footer.insert(footer.end(), mlp, mlp + 4);
                footer.insert(footer.end(), min_s.begin(), min_s.end());
                auto *mxp = reinterpret_cast<uint8_t *>(&max_len);
                footer.insert(footer.end(), mxp, mxp + 4);
                footer.insert(footer.end(), max_s.begin(), max_s.end());
            }
        }
    }

    file_.write(reinterpret_cast<const char *>(footer.data()), footer.size());
    uint32_t footer_size = static_cast<uint32_t>(footer.size());
    file_.write(reinterpret_cast<const char *>(&footer_size), 4);
    file_.write(PARQUET_MAGIC, 4);
    file_.close();
}

// ============================================================================
// Parquet Reader
// ============================================================================

ParquetReader::ParquetReader(const std::string &path) : path_(path) {
    ReadMetadata();
}

void ParquetReader::ReadMetadata() {
    std::ifstream file(path_, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Parquet file: " + path_);

    auto file_size = file.tellg();
    if (file_size < 12) throw IOException(ErrorCode::CORRUPT_DATA, "File too small for Parquet");

    // Read magic at start.
    file.seekg(0);
    char magic_start[4];
    file.read(magic_start, 4);
    if (std::memcmp(magic_start, PARQUET_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not a Parquet file (bad magic)");

    // Read footer size and magic at end.
    file.seekg(static_cast<std::streamoff>(file_size) - 8);
    uint32_t footer_size;
    file.read(reinterpret_cast<char *>(&footer_size), 4);
    char magic_end[4];
    file.read(magic_end, 4);
    if (std::memcmp(magic_end, PARQUET_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not a Parquet file (bad end magic)");

    // Read footer.
    // Validate footer_size against file bounds.
    if (footer_size > file_size - 8)
        throw IOException(ErrorCode::CORRUPT_DATA, "Parquet footer size exceeds file");
    if (footer_size < 16)
        throw IOException(ErrorCode::CORRUPT_DATA, "Parquet footer too small");

    auto footer_offset = static_cast<std::streamoff>(file_size) - 8 - footer_size;
    std::vector<uint8_t> footer(footer_size);
    file.seekg(footer_offset);
    file.read(reinterpret_cast<char *>(footer.data()), footer_size);

    // Bounds-checked read helper.
    auto safe_read = [&](size_t &p, void *dst, size_t n) {
        if (p + n > footer.size())
            throw IOException(ErrorCode::CORRUPT_DATA, "Parquet footer truncated");
        std::memcpy(dst, &footer[p], n);
        p += n;
    };

    // Parse footer.
    size_t pos = 0;
    uint32_t num_rg;
    safe_read(pos, &num_rg, 4);

    safe_read(pos, &meta_.num_rows, 8);

    uint32_t num_cols;
    std::memcpy(&num_cols, &footer[pos], 4); pos += 4;

    for (uint32_t c = 0; c < num_cols; c++) {
        uint32_t name_len;
        std::memcpy(&name_len, &footer[pos], 4); pos += 4;
        meta_.column_names.emplace_back(reinterpret_cast<const char *>(&footer[pos]), name_len);
        pos += name_len;

        int32_t type_id;
        std::memcpy(&type_id, &footer[pos], 4); pos += 4;
        meta_.column_types.push_back(ParquetToLogicalType(static_cast<ParquetType>(type_id)));
    }

    for (uint32_t r = 0; r < num_rg; r++) {
        ParquetRowGroup rg;
        std::memcpy(&rg.num_rows, &footer[pos], 8); pos += 8;

        for (uint32_t c = 0; c < num_cols; c++) {
            ParquetColumnMeta col;
            col.name = meta_.column_names[c];
            col.slothdb_type = meta_.column_types[c];
            col.parquet_type = LogicalToParquetType(meta_.column_types[c]);

            std::memcpy(&col.data_offset, &footer[pos], 8); pos += 8;
            std::memcpy(&col.data_size, &footer[pos], 8); pos += 8;

            col.has_stats = footer[pos++] != 0;
            if (col.has_stats) {
                uint32_t min_len;
                std::memcpy(&min_len, &footer[pos], 4); pos += 4;
                auto min_str = std::string(reinterpret_cast<const char *>(&footer[pos]), min_len);
                pos += min_len;
                uint32_t max_len;
                std::memcpy(&max_len, &footer[pos], 4); pos += 4;
                auto max_str = std::string(reinterpret_cast<const char *>(&footer[pos]), max_len);
                pos += max_len;

                // Reconstruct min/max values.
                auto &type = meta_.column_types[c];
                try {
                    if (type.id() == LogicalTypeId::INTEGER) {
                        col.min_value = Value::INTEGER(std::stoi(min_str));
                        col.max_value = Value::INTEGER(std::stoi(max_str));
                    } else if (type.id() == LogicalTypeId::BIGINT) {
                        col.min_value = Value::BIGINT(std::stoll(min_str));
                        col.max_value = Value::BIGINT(std::stoll(max_str));
                    } else if (type.id() == LogicalTypeId::DOUBLE) {
                        col.min_value = Value::DOUBLE(std::stod(min_str));
                        col.max_value = Value::DOUBLE(std::stod(max_str));
                    } else {
                        col.min_value = Value::VARCHAR(min_str);
                        col.max_value = Value::VARCHAR(max_str);
                    }
                } catch (...) {
                    col.has_stats = false;
                }
            }

            col.num_values = rg.num_rows;
            rg.columns.push_back(std::move(col));
        }
        meta_.row_groups.push_back(std::move(rg));
    }

    meta_read_ = true;
}

std::vector<Value> ParquetReader::ReadColumnChunk(const ParquetColumnMeta &meta) {
    std::ifstream file(path_, std::ios::binary);
    file.seekg(meta.data_offset);

    std::vector<uint8_t> data(meta.data_size);
    file.read(reinterpret_cast<char *>(data.data()), meta.data_size);

    std::vector<Value> values;
    size_t pos = 0;

    for (int64_t i = 0; i < meta.num_values && pos < data.size(); i++) {
        uint32_t null_marker;
        std::memcpy(&null_marker, &data[pos], 4); pos += 4;

        if (null_marker == 0xFFFFFFFF) {
            values.push_back(Value());
            continue;
        }

        switch (meta.parquet_type) {
        case ParquetType::BOOLEAN: {
            values.push_back(Value::BOOLEAN(data[pos++] != 0));
            break;
        }
        case ParquetType::INT32: {
            int32_t val;
            std::memcpy(&val, &data[pos], 4); pos += 4;
            values.push_back(Value::INTEGER(val));
            break;
        }
        case ParquetType::INT64: {
            int64_t val;
            std::memcpy(&val, &data[pos], 8); pos += 8;
            values.push_back(Value::BIGINT(val));
            break;
        }
        case ParquetType::FLOAT: {
            float val;
            std::memcpy(&val, &data[pos], 4); pos += 4;
            values.push_back(Value::FLOAT(val));
            break;
        }
        case ParquetType::DOUBLE: {
            double val;
            std::memcpy(&val, &data[pos], 8); pos += 8;
            values.push_back(Value::DOUBLE(val));
            break;
        }
        case ParquetType::BYTE_ARRAY: {
            uint32_t len;
            std::memcpy(&len, &data[pos], 4); pos += 4;
            values.push_back(Value::VARCHAR(
                std::string(reinterpret_cast<const char *>(&data[pos]), len)));
            pos += len;
            break;
        }
        }
    }

    return values;
}

std::vector<std::vector<Value>> ParquetReader::ReadRowGroup(idx_t rg_idx) {
    if (rg_idx >= meta_.row_groups.size()) return {};

    auto &rg = meta_.row_groups[rg_idx];
    idx_t num_cols = rg.columns.size();

    // Read each column.
    std::vector<std::vector<Value>> columns;
    for (idx_t c = 0; c < num_cols; c++) {
        columns.push_back(ReadColumnChunk(rg.columns[c]));
    }

    // Transpose: columns to rows.
    std::vector<std::vector<Value>> rows;
    for (int64_t r = 0; r < rg.num_rows; r++) {
        std::vector<Value> row;
        for (idx_t c = 0; c < num_cols; c++) {
            row.push_back(r < static_cast<int64_t>(columns[c].size())
                ? columns[c][r] : Value());
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::vector<Value>> ParquetReader::ReadAll() {
    std::vector<std::vector<Value>> all_rows;
    for (idx_t rg = 0; rg < meta_.row_groups.size(); rg++) {
        auto rows = ReadRowGroup(rg);
        all_rows.insert(all_rows.end(), rows.begin(), rows.end());
    }
    return all_rows;
}

bool ParquetReader::RowGroupMightMatch(idx_t rg_idx, idx_t col_idx,
                                        const std::string &op, const Value &val) const {
    if (rg_idx >= meta_.row_groups.size()) return false;
    auto &rg = meta_.row_groups[rg_idx];
    if (col_idx >= rg.columns.size()) return true;
    auto &col = rg.columns[col_idx];
    if (!col.has_stats) return true;

    // Use zone map logic.
    if (op == "=" || op == "==") {
        return !(val < col.min_value) && !(col.max_value < val);
    } else if (op == ">") {
        return !(col.max_value < val) && !(col.max_value == val);
    } else if (op == ">=") {
        return !(col.max_value < val);
    } else if (op == "<") {
        return !(val < col.min_value) && !(col.min_value == val);
    } else if (op == "<=") {
        return !(val < col.min_value);
    }
    return true;
}

} // namespace slothdb

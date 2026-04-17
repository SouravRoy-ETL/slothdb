#include "slothdb/storage/csv_reader.hpp"
#include "slothdb/storage/data_table.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include <sstream>

namespace slothdb {

// ============================================================================
// CSV Reader
// ============================================================================

CSVReader::CSVReader(const std::string &path, CSVOptions options)
    : options_(std::move(options)) {
    file_.open(path);
    if (!file_.is_open()) {
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open CSV file: " + path);
    }
}

std::vector<std::string> CSVReader::ParseLine() {
    std::vector<std::string> fields;
    std::string line;
    if (!std::getline(file_, line)) {
        eof_ = true;
        return fields;
    }
    // Remove trailing \r if present (Windows line endings).
    if (!line.empty() && line.back() == '\r') line.pop_back();

    size_t pos = 0;
    while (pos <= line.size()) {
        std::string field;

        if (pos < line.size() && line[pos] == options_.quote) {
            // Quoted field.
            pos++; // skip opening quote
            while (pos < line.size()) {
                if (line[pos] == options_.escape && pos + 1 < line.size() &&
                    line[pos + 1] == options_.quote) {
                    field += options_.quote;
                    pos += 2;
                } else if (line[pos] == options_.quote) {
                    pos++; // skip closing quote
                    break;
                } else {
                    field += line[pos++];
                }
            }
            // Skip delimiter after closing quote.
            if (pos < line.size() && line[pos] == options_.delimiter) pos++;
        } else {
            // Unquoted field.
            size_t end = line.find(options_.delimiter, pos);
            if (end == std::string::npos) {
                field = line.substr(pos);
                pos = line.size() + 1;
            } else {
                field = line.substr(pos, end - pos);
                pos = end + 1;
            }
        }
        fields.push_back(field);
    }
    return fields;
}

std::vector<std::string> CSVReader::ReadHeader() {
    if (options_.header && !header_read_) {
        header_read_ = true;
        return ParseLine();
    }
    return {};
}

std::vector<LogicalType> CSVReader::DetectTypes(idx_t sample_size) {
    // Save position.
    auto start_pos = file_.tellg();

    // Skip header if needed.
    if (options_.header && !header_read_) {
        ReadHeader();
    }

    std::vector<LogicalType> types;
    idx_t rows_read = 0;

    while (rows_read < sample_size && !eof_) {
        auto fields = ParseLine();
        if (fields.empty()) break;

        if (types.empty()) {
            types.resize(fields.size(), LogicalType::VARCHAR());
        }

        for (size_t i = 0; i < fields.size() && i < types.size(); i++) {
            auto &f = fields[i];
            if (f.empty() || f == options_.null_string) continue;

            // Try integer.
            try {
                (void)std::stoll(f);
                if (types[i].id() == LogicalTypeId::VARCHAR) {
                    types[i] = LogicalType::BIGINT();
                }
                continue;
            } catch (...) {}

            // Try double.
            try {
                (void)std::stod(f);
                if (types[i].id() == LogicalTypeId::VARCHAR ||
                    types[i].id() == LogicalTypeId::BIGINT) {
                    types[i] = LogicalType::DOUBLE();
                }
                continue;
            } catch (...) {}

            // Default: VARCHAR.
            types[i] = LogicalType::VARCHAR();
        }
        rows_read++;
    }

    // Reset to start.
    file_.clear();
    file_.seekg(start_pos);
    eof_ = false;
    header_read_ = false;

    return types;
}

Value CSVReader::ConvertValue(const std::string &str, const LogicalType &type) {
    if (str.empty() || str == options_.null_string) return Value();

    try {
        switch (type.id()) {
        case LogicalTypeId::INTEGER:
            return Value::INTEGER(std::stoi(str));
        case LogicalTypeId::BIGINT:
            return Value::BIGINT(std::stoll(str));
        case LogicalTypeId::DOUBLE:
            return Value::DOUBLE(std::stod(str));
        case LogicalTypeId::FLOAT:
            return Value::FLOAT(std::stof(str));
        case LogicalTypeId::BOOLEAN:
            return Value::BOOLEAN(str == "true" || str == "1" || str == "TRUE");
        case LogicalTypeId::VARCHAR:
        default:
            return Value::VARCHAR(str);
        }
    } catch (...) {
        return Value::VARCHAR(str);
    }
}

std::vector<std::vector<Value>> CSVReader::ReadAll(const std::vector<LogicalType> &types) {
    std::vector<std::vector<Value>> rows;
    if (options_.header && !header_read_) ReadHeader();

    while (!eof_) {
        auto fields = ParseLine();
        if (fields.empty()) break;

        std::vector<Value> row;
        for (size_t i = 0; i < types.size(); i++) {
            if (i < fields.size()) {
                row.push_back(ConvertValue(fields[i], types[i]));
            } else {
                row.push_back(Value());
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::vector<Value>> CSVReader::ReadBatch(const std::vector<LogicalType> &types,
                                                       idx_t batch_size) {
    std::vector<std::vector<Value>> rows;
    if (options_.header && !header_read_) ReadHeader();

    for (idx_t r = 0; r < batch_size && !eof_; r++) {
        auto fields = ParseLine();
        if (fields.empty()) break;

        std::vector<Value> row;
        for (size_t i = 0; i < types.size(); i++) {
            if (i < fields.size()) {
                row.push_back(ConvertValue(fields[i], types[i]));
            } else {
                row.push_back(Value());
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void CSVReader::SetValueDirect(DataChunk &chunk, idx_t col, idx_t row,
                                const std::string &str, const LogicalType &type) {
    if (str.empty() || str == options_.null_string) {
        chunk.SetValue(col, row, Value());
        return;
    }
    try {
        switch (type.id()) {
        case LogicalTypeId::INTEGER:
            chunk.SetValue(col, row, Value::INTEGER(std::stoi(str)));
            return;
        case LogicalTypeId::BIGINT:
            chunk.SetValue(col, row, Value::BIGINT(std::stoll(str)));
            return;
        case LogicalTypeId::DOUBLE:
            chunk.SetValue(col, row, Value::DOUBLE(std::stod(str)));
            return;
        case LogicalTypeId::FLOAT:
            chunk.SetValue(col, row, Value::FLOAT(std::stof(str)));
            return;
        case LogicalTypeId::BOOLEAN:
            chunk.SetValue(col, row, Value::BOOLEAN(str == "true" || str == "1" || str == "TRUE"));
            return;
        default:
            chunk.SetValue(col, row, Value::VARCHAR(str));
            return;
        }
    } catch (...) {
        chunk.SetValue(col, row, Value::VARCHAR(str));
    }
}

idx_t CSVReader::ReadChunk(DataChunk &chunk, const std::vector<LogicalType> &types) {
    if (options_.header && !header_read_) ReadHeader();
    chunk.Reset();

    idx_t row_count = 0;
    while (row_count < VECTOR_SIZE && !eof_) {
        auto fields = ParseLine();
        if (fields.empty()) break;

        for (idx_t c = 0; c < static_cast<idx_t>(types.size()); c++) {
            if (c < static_cast<idx_t>(fields.size())) {
                SetValueDirect(chunk, c, row_count, fields[c], types[c]);
            } else {
                chunk.SetValue(c, row_count, Value());
            }
        }
        row_count++;
    }

    chunk.SetCardinality(row_count);
    return row_count;
}

void CSVReader::ReadIntoTable(DataTable &table, const std::vector<LogicalType> &types) {
    DataChunk chunk;
    chunk.Initialize(types);

    while (!eof_) {
        idx_t count = ReadChunk(chunk, types);
        if (count == 0) break;
        table.Append(chunk);
    }
}

// ============================================================================
// CSV Writer
// ============================================================================

CSVWriter::CSVWriter(const std::string &path, CSVOptions options)
    : options_(std::move(options)) {
    file_.open(path);
    if (!file_.is_open()) {
        throw IOException(ErrorCode::FILE_WRITE_ERROR, "Cannot create CSV file: " + path);
    }
}

void CSVWriter::WriteHeader(const std::vector<std::string> &columns) {
    for (size_t i = 0; i < columns.size(); i++) {
        if (i > 0) file_ << options_.delimiter;
        file_ << columns[i];
    }
    file_ << "\n";
}

void CSVWriter::WriteRow(const std::vector<Value> &row) {
    for (size_t i = 0; i < row.size(); i++) {
        if (i > 0) file_ << options_.delimiter;
        if (row[i].IsNull()) {
            file_ << options_.null_string;
        } else {
            auto str = row[i].ToString();
            // Quote if contains delimiter, quote, newline, or starts with formula chars.
            bool needs_quote = str.find(options_.delimiter) != std::string::npos ||
                               str.find(options_.quote) != std::string::npos ||
                               str.find('\n') != std::string::npos ||
                               (!str.empty() && (str[0] == '=' || str[0] == '+' ||
                                str[0] == '-' || str[0] == '@'));
            if (needs_quote) {
                file_ << options_.quote;
                for (char c : str) {
                    if (c == options_.quote) file_ << options_.escape;
                    file_ << c;
                }
                file_ << options_.quote;
            } else {
                file_ << str;
            }
        }
    }
    file_ << "\n";
}

void CSVWriter::Flush() {
    file_.flush();
}

} // namespace slothdb

#include "slothdb/storage/json_reader.hpp"
#include "slothdb/common/exception.hpp"
#include <sstream>
#include <algorithm>

namespace slothdb {

// ============================================================================
// JSON Parser (minimal, zero-dependency)
// ============================================================================

void JSONParser::SkipWhitespace(const std::string &json, size_t &pos) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
           json[pos] == '\n' || json[pos] == '\r')) pos++;
}

Value JSONParser::Parse(const std::string &json, size_t &pos) {
    SkipWhitespace(json, pos);
    if (pos >= json.size()) return Value();

    char c = json[pos];
    if (c == '"') return ParseString(json, pos);
    if (c == '{') return ParseObject(json, pos);
    if (c == '[') return ParseArray(json, pos);
    if (c == 't') { pos += 4; return Value::BOOLEAN(true); }
    if (c == 'f') { pos += 5; return Value::BOOLEAN(false); }
    if (c == 'n') { pos += 4; return Value(); } // null
    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(json, pos);

    return Value();
}

Value JSONParser::ParseString(const std::string &json, size_t &pos) {
    pos++; // skip opening "
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'n': result += '\n'; break;
            case 't': result += '\t'; break;
            case 'r': result += '\r'; break;
            default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    if (pos < json.size()) pos++; // skip closing "
    return Value::VARCHAR(result);
}

Value JSONParser::ParseNumber(const std::string &json, size_t &pos) {
    size_t start = pos;
    bool is_float = false;
    if (json[pos] == '-') pos++;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') pos++;
    if (pos < json.size() && json[pos] == '.') { is_float = true; pos++; }
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') pos++;
    if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
        is_float = true; pos++;
        if (pos < json.size() && (json[pos] == '+' || json[pos] == '-')) pos++;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') pos++;
    }

    auto num_str = json.substr(start, pos - start);
    if (is_float) return Value::DOUBLE(std::stod(num_str));

    int64_t val = std::stoll(num_str);
    if (val >= INT32_MIN && val <= INT32_MAX) return Value::INTEGER(static_cast<int32_t>(val));
    return Value::BIGINT(val);
}

Value JSONParser::ParseObject(const std::string &json, size_t &pos) {
    pos++; // skip {
    // Return as VARCHAR with the JSON representation for now.
    // For table-level parsing, we extract keys directly in JSONReader.
    int depth = 1;
    size_t start = pos;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '{') depth++;
        else if (json[pos] == '}') depth--;
        if (depth > 0) pos++;
    }
    auto obj_str = "{" + json.substr(start, pos - start) + "}";
    if (pos < json.size()) pos++; // skip }
    return Value::VARCHAR(obj_str);
}

Value JSONParser::ParseArray(const std::string &json, size_t &pos) {
    pos++; // skip [
    int depth = 1;
    size_t start = pos;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '[') depth++;
        else if (json[pos] == ']') depth--;
        if (depth > 0) pos++;
    }
    auto arr_str = "[" + json.substr(start, pos - start) + "]";
    if (pos < json.size()) pos++; // skip ]
    return Value::VARCHAR(arr_str);
}

// ============================================================================
// JSON Reader
// ============================================================================

JSONReader::JSONReader(const std::string &path, JSONOptions options)
    : path_(path), options_(options) {}

void JSONReader::ParseNDJSON() {
    std::ifstream file(path_);
    if (!file.is_open()) throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open: " + path_);

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] != '{') continue;

        // Parse object manually to extract key-value pairs.
        std::unordered_map<std::string, Value> record;
        size_t pos = 1; // skip {
        JSONParser::SkipWhitespace(line, pos);

        while (pos < line.size() && line[pos] != '}') {
            // Parse key.
            if (line[pos] != '"') break;
            auto key_val = JSONParser::ParseString(line, pos);
            auto key = key_val.GetValue<std::string>();

            JSONParser::SkipWhitespace(line, pos);
            if (pos < line.size() && line[pos] == ':') pos++;
            JSONParser::SkipWhitespace(line, pos);

            // Parse value.
            auto val = JSONParser::Parse(line, pos);
            record[key] = val;

            JSONParser::SkipWhitespace(line, pos);
            if (pos < line.size() && line[pos] == ',') pos++;
            JSONParser::SkipWhitespace(line, pos);
        }

        records_.push_back(std::move(record));
    }
    parsed_ = true;
}

void JSONReader::ParseJSONArray() {
    std::ifstream file(path_);
    if (!file.is_open()) throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open: " + path_);

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    size_t pos = 0;
    JSONParser::SkipWhitespace(content, pos);

    if (pos >= content.size() || content[pos] != '[') {
        // Try as NDJSON.
        options_.array_format = false;
        file.close();
        ParseNDJSON();
        return;
    }

    pos++; // skip [
    JSONParser::SkipWhitespace(content, pos);

    while (pos < content.size() && content[pos] != ']') {
        if (content[pos] != '{') break;

        // Parse object.
        std::unordered_map<std::string, Value> record;
        pos++; // skip {
        JSONParser::SkipWhitespace(content, pos);

        while (pos < content.size() && content[pos] != '}') {
            if (content[pos] != '"') break;
            auto key_val = JSONParser::ParseString(content, pos);
            auto key = key_val.GetValue<std::string>();

            JSONParser::SkipWhitespace(content, pos);
            if (pos < content.size() && content[pos] == ':') pos++;
            JSONParser::SkipWhitespace(content, pos);

            auto val = JSONParser::Parse(content, pos);
            record[key] = val;

            JSONParser::SkipWhitespace(content, pos);
            if (pos < content.size() && content[pos] == ',') pos++;
            JSONParser::SkipWhitespace(content, pos);
        }

        if (pos < content.size() && content[pos] == '}') pos++;
        records_.push_back(std::move(record));

        JSONParser::SkipWhitespace(content, pos);
        if (pos < content.size() && content[pos] == ',') pos++;
        JSONParser::SkipWhitespace(content, pos);
    }
    parsed_ = true;
}

void JSONReader::DetectSchema() {
    if (!parsed_) {
        if (options_.array_format) ParseJSONArray();
        else ParseNDJSON();
    }

    // Collect all keys across all records.
    std::vector<std::string> keys;
    std::unordered_map<std::string, LogicalType> type_map;

    for (auto &record : records_) {
        for (auto &[key, val] : record) {
            if (type_map.find(key) == type_map.end()) {
                keys.push_back(key);
                type_map[key] = val.IsNull() ? LogicalType::VARCHAR() : val.type();
            } else if (!val.IsNull()) {
                // Widen type if needed.
                auto cur = type_map[key];
                auto new_type = val.type();
                if (cur.id() != new_type.id()) {
                    // Default to VARCHAR for mixed types.
                    if (cur.id() == LogicalTypeId::INTEGER && new_type.id() == LogicalTypeId::DOUBLE)
                        type_map[key] = LogicalType::DOUBLE();
                    else if (cur.id() != LogicalTypeId::VARCHAR && new_type.id() != LogicalTypeId::VARCHAR)
                        type_map[key] = LogicalType::DOUBLE();
                    else
                        type_map[key] = LogicalType::VARCHAR();
                }
            }
        }
    }

    column_names_ = keys;
    column_types_.clear();
    for (auto &key : keys) {
        column_types_.push_back(type_map[key]);
    }
}

std::vector<std::vector<Value>> JSONReader::ReadAll() {
    if (!parsed_) DetectSchema();

    std::vector<std::vector<Value>> rows;
    for (auto &record : records_) {
        std::vector<Value> row;
        for (auto &key : column_names_) {
            auto it = record.find(key);
            if (it != record.end()) {
                row.push_back(it->second);
            } else {
                row.push_back(Value());
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// ============================================================================
// JSON Writer
// ============================================================================

JSONWriter::JSONWriter(const std::string &path, JSONOptions options)
    : options_(options) {
    file_.open(path);
    if (!file_.is_open()) throw IOException(ErrorCode::FILE_WRITE_ERROR, "Cannot create: " + path);
    if (options_.array_format) file_ << "[\n";
}

void JSONWriter::WriteHeader(const std::vector<std::string> &columns) {
    columns_ = columns;
}

std::string JSONWriter::ValueToJSON(const Value &val) {
    if (val.IsNull()) return "null";
    switch (val.type().id()) {
    case LogicalTypeId::BOOLEAN:
        return val.GetValue<bool>() ? "true" : "false";
    case LogicalTypeId::INTEGER:
        return std::to_string(val.GetValue<int32_t>());
    case LogicalTypeId::BIGINT:
        return std::to_string(val.GetValue<int64_t>());
    case LogicalTypeId::FLOAT:
        return std::to_string(val.GetValue<float>());
    case LogicalTypeId::DOUBLE:
        return std::to_string(val.GetValue<double>());
    case LogicalTypeId::VARCHAR: {
        // Escape string for JSON.
        auto s = val.GetValue<std::string>();
        std::string escaped = "\"";
        for (char c : s) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else if (c == '\n') escaped += "\\n";
            else if (c == '\t') escaped += "\\t";
            else escaped += c;
        }
        escaped += "\"";
        return escaped;
    }
    default:
        return "\"" + val.ToString() + "\"";
    }
}

void JSONWriter::WriteRow(const std::vector<std::string> &columns,
                           const std::vector<Value> &row) {
    if (options_.array_format && !first_row_) file_ << ",\n";
    first_row_ = false;

    file_ << "  {";
    for (size_t i = 0; i < columns.size() && i < row.size(); i++) {
        if (i > 0) file_ << ", ";
        file_ << "\"" << columns[i] << "\": " << ValueToJSON(row[i]);
    }
    file_ << "}";

    if (!options_.array_format) file_ << "\n"; // NDJSON
}

void JSONWriter::Finish() {
    if (options_.array_format) file_ << "\n]\n";
    file_.flush();
}

} // namespace slothdb

#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>

namespace slothdb {

struct JSONOptions {
    bool array_format = true;  // true=JSON array, false=NDJSON (one object per line)
    bool auto_detect = true;
};

// Minimal JSON parser — handles objects, arrays, strings, numbers, booleans, null.
class JSONParser {
public:
    // Parse a JSON value from a string. Returns the parsed value and updates pos.
    static Value Parse(const std::string &json, size_t &pos);
    static Value ParseString(const std::string &json, size_t &pos);
    static Value ParseNumber(const std::string &json, size_t &pos);
    static Value ParseObject(const std::string &json, size_t &pos);
    static Value ParseArray(const std::string &json, size_t &pos);
    static void SkipWhitespace(const std::string &json, size_t &pos);
};

// JSON Reader: read JSON/NDJSON files into rows.
class JSONReader {
public:
    JSONReader(const std::string &path, JSONOptions options = {});

    // Detect column names and types from the data.
    void DetectSchema();

    // Get detected column names and types.
    const std::vector<std::string> &GetColumnNames() const { return column_names_; }
    const std::vector<LogicalType> &GetColumnTypes() const { return column_types_; }

    // Read all rows.
    std::vector<std::vector<Value>> ReadAll();

private:
    void ParseNDJSON();
    void ParseJSONArray();

    std::string path_;
    JSONOptions options_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    std::vector<std::unordered_map<std::string, Value>> records_;
    bool parsed_ = false;
};

// JSON Writer: write rows to JSON/NDJSON.
class JSONWriter {
public:
    JSONWriter(const std::string &path, JSONOptions options = {});

    void WriteHeader(const std::vector<std::string> &columns);
    void WriteRow(const std::vector<std::string> &columns, const std::vector<Value> &row);
    void Finish();

private:
    std::string ValueToJSON(const Value &val);

    std::ofstream file_;
    JSONOptions options_;
    bool first_row_ = true;
    std::vector<std::string> columns_;
};

} // namespace slothdb

#include "slothdb/storage/json_reader.hpp"
#include "slothdb/storage/data_table.hpp"
#include "slothdb/storage/row_group.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/string_type.hpp"
#include "slothdb/common/exception.hpp"
#include <sstream>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// Fast NDJSON parser: single pass over the mmap'd/loaded buffer, no per-line
// std::unordered_map, no per-char std::string append. Detects schema from the
// first record; subsequent records are parsed positionally (with a hash-like
// fallback only when key order differs). This is ~20-40× faster than the
// umap path for 1 M+ row NDJSON.
static inline void fast_skip_ws(const char *&p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
}

namespace {
// Read a quoted JSON string into a std::string; handles simple escapes. Returns
// past the closing quote.
const char *fast_parse_string(const char *p, const char *end, std::string &out) {
    out.clear();
    p++; // skip opening "
    const char *run_start = p;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) {
            out.append(run_start, p - run_start);
            p++;
            switch (*p) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'n':  out.push_back('\n'); break;
            case 't':  out.push_back('\t'); break;
            case 'r':  out.push_back('\r'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            default:   out.push_back(*p);   break;
            }
            p++;
            run_start = p;
        } else {
            p++;
        }
    }
    out.append(run_start, p - run_start);
    if (p < end) p++; // skip closing "
    return p;
}

// Parse a number literal; returns past the last digit/char.
const char *fast_parse_number_raw(const char *p, const char *end,
                                   const char *&out_start, const char *&out_end,
                                   bool &is_float) {
    out_start = p;
    is_float = false;
    if (p < end && *p == '-') p++;
    while (p < end && *p >= '0' && *p <= '9') p++;
    if (p < end && *p == '.') { is_float = true; p++;
        while (p < end && *p >= '0' && *p <= '9') p++; }
    if (p < end && (*p == 'e' || *p == 'E')) { is_float = true; p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        while (p < end && *p >= '0' && *p <= '9') p++; }
    out_end = p;
    return p;
}

// Assign a parsed value to a row slot based on the column's declared type.
void assign_value(Value &slot, LogicalTypeId tid, const char *v_start,
                  const char *v_end, bool is_string, bool is_null) {
    if (is_null) { slot = Value(); return; }
    if (is_string) {
        // v_start..v_end points at the decoded string bytes.
        slot = Value::VARCHAR(std::string(v_start, v_end - v_start));
        return;
    }
    // Numeric literal — parse per declared type.
    switch (tid) {
    case LogicalTypeId::INTEGER: {
        int32_t i = 0; std::from_chars(v_start, v_end, i);
        slot = Value::INTEGER(i); break;
    }
    case LogicalTypeId::BIGINT: {
        int64_t i = 0; std::from_chars(v_start, v_end, i);
        slot = Value::BIGINT(i); break;
    }
    case LogicalTypeId::DOUBLE: {
        double d = 0; std::from_chars(v_start, v_end, d);
        slot = Value::DOUBLE(d); break;
    }
    case LogicalTypeId::FLOAT: {
        double d = 0; std::from_chars(v_start, v_end, d);
        slot = Value::FLOAT((float)d); break;
    }
    default: {
        // VARCHAR column receiving a numeric literal — keep as string.
        slot = Value::VARCHAR(std::string(v_start, v_end - v_start));
        break;
    }
    }
}
} // namespace

void JSONReader::ParseNDJSON() {
    std::ifstream file(path_, std::ios::binary | std::ios::ate);
    if (!file.is_open()) throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open: " + path_);
    auto sz = static_cast<size_t>(file.tellg());
    if (sz == 0) { parsed_ = true; return; }
    std::string buf(sz, '\0');
    file.seekg(0);
    file.read(buf.data(), sz);

    const char *data = buf.data();
    const char *end = data + sz;
    const char *p = data;

    // --- Pass 1: parse first object to detect schema (key order + types).
    while (p < end && *p != '{') p++;
    if (p >= end) { parsed_ = true; return; }

    column_names_.clear();
    column_types_.clear();
    std::vector<Value> first_row;

    p++; // skip {
    std::string key, strbuf;
    while (p < end && *p != '}') {
        fast_skip_ws(p, end);
        if (p >= end || *p == '}') break;
        if (*p != '"') break;
        p = fast_parse_string(p, end, key);
        fast_skip_ws(p, end);
        if (p < end && *p == ':') p++;
        fast_skip_ws(p, end);
        // Parse value → remember type for schema.
        LogicalType t = LogicalType::VARCHAR();
        Value v;
        if (p < end && *p == '"') {
            p = fast_parse_string(p, end, strbuf);
            v = Value::VARCHAR(strbuf);
            t = LogicalType::VARCHAR();
        } else if (p < end && *p == 'n') {
            p += 4; v = Value(); t = LogicalType::VARCHAR();
        } else if (p < end && *p == 't') {
            p += 4; v = Value::BOOLEAN(true); t = LogicalType::BOOLEAN();
        } else if (p < end && *p == 'f') {
            p += 5; v = Value::BOOLEAN(false); t = LogicalType::BOOLEAN();
        } else {
            const char *ns, *ne; bool isf;
            p = fast_parse_number_raw(p, end, ns, ne, isf);
            if (isf) { double d = 0; std::from_chars(ns, ne, d); v = Value::DOUBLE(d); t = LogicalType::DOUBLE(); }
            else {
                int64_t i = 0; std::from_chars(ns, ne, i);
                if (i >= INT32_MIN && i <= INT32_MAX) { v = Value::INTEGER((int32_t)i); t = LogicalType::INTEGER(); }
                else { v = Value::BIGINT(i); t = LogicalType::BIGINT(); }
            }
        }
        column_names_.push_back(key);
        column_types_.push_back(t);
        first_row.push_back(std::move(v));
        fast_skip_ws(p, end);
        if (p < end && *p == ',') p++;
    }
    if (p < end && *p == '}') p++;
    rows_.reserve(1 << 14);
    rows_.push_back(std::move(first_row));

    // Widen INTEGER to BIGINT/DOUBLE if later rows overflow — pragmatic: if
    // revenue-like columns arrive with non-integer later, we rely on
    // from_chars-to-double which tolerates ints. Keep this simple for now.

    // --- Pass 2: parse remaining records into rows_ positionally.
    const idx_t ncols = column_names_.size();
    while (p < end) {
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        p++; // skip {
        std::vector<Value> row(ncols);
        idx_t next_expected = 0;
        while (p < end && *p != '}') {
            fast_skip_ws(p, end);
            if (p >= end || *p == '}') break;
            if (*p != '"') {
                // skip to end-of-line; record is malformed
                while (p < end && *p != '\n') p++;
                break;
            }
            // Match key inline against canonical column list (positional fast
            // path, fall back to linear scan when out-of-order).
            p++; // skip opening "
            const char *key_start = p;
            while (p < end && *p != '"') p++;
            size_t key_len = (size_t)(p - key_start);
            if (p < end) p++; // skip closing "
            idx_t col = static_cast<idx_t>(-1);
            if (next_expected < ncols &&
                column_names_[next_expected].size() == key_len &&
                std::memcmp(column_names_[next_expected].data(),
                             key_start, key_len) == 0) {
                col = next_expected;
            } else {
                for (idx_t i = 0; i < ncols; i++) {
                    if (column_names_[i].size() == key_len &&
                        std::memcmp(column_names_[i].data(), key_start, key_len) == 0) {
                        col = i; break;
                    }
                }
            }
            fast_skip_ws(p, end);
            if (p < end && *p == ':') p++;
            fast_skip_ws(p, end);
            // Parse value.
            if (p < end && *p == '"') {
                p = fast_parse_string(p, end, strbuf);
                if (col != static_cast<idx_t>(-1)) {
                    row[col] = Value::VARCHAR(strbuf);
                }
            } else if (p < end && *p == 'n') {
                p += 4;
                if (col != static_cast<idx_t>(-1)) row[col] = Value();
            } else if (p < end && *p == 't') {
                p += 4;
                if (col != static_cast<idx_t>(-1)) row[col] = Value::BOOLEAN(true);
            } else if (p < end && *p == 'f') {
                p += 5;
                if (col != static_cast<idx_t>(-1)) row[col] = Value::BOOLEAN(false);
            } else {
                const char *ns, *ne; bool isf;
                p = fast_parse_number_raw(p, end, ns, ne, isf);
                if (col != static_cast<idx_t>(-1)) {
                    assign_value(row[col], column_types_[col].id(), ns, ne, false, false);
                }
            }
            if (col != static_cast<idx_t>(-1)) next_expected = col + 1;
            fast_skip_ws(p, end);
            if (p < end && *p == ',') p++;
        }
        if (p < end && *p == '}') p++;
        rows_.push_back(std::move(row));
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

    // Fast NDJSON path already populated column_names_/column_types_ via its
    // first-record scan; nothing more to do.
    if (!rows_.empty() && !column_names_.empty()) return;

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

    // Sort keys for deterministic column order across platforms.
    std::sort(keys.begin(), keys.end());
    column_names_ = keys;
    column_types_.clear();
    for (auto &key : keys) {
        column_types_.push_back(type_map[key]);
    }
}

// ============================================================================
// Streaming NDJSON → DataTable. Parses directly into DataChunk column vectors
// (typed memcpy / inline construct for numerics; string_t + VectorStringBuffer
// for VARCHAR), skipping the rows_ intermediate and per-cell Value boxing.
// ============================================================================
// Detect the NDJSON schema by parsing only the first record. Leaves the rest
// of the file unparsed — callers that want the data should use
// ReadIntoTable() or ParseNDJSON() afterwards.
static void detect_ndjson_schema_from_first(const std::string &path,
                                             std::vector<std::string> &col_names,
                                             std::vector<LogicalType> &col_types) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open: " + path);
    auto sz = static_cast<size_t>(f.tellg());
    if (sz == 0) return;
    // Read enough to capture the first object — 64 KB is plenty for any
    // reasonable NDJSON record.
    size_t head = std::min<size_t>(sz, 64 * 1024);
    std::string buf(head, '\0');
    f.seekg(0);
    f.read(buf.data(), head);
    const char *p = buf.data(), *end = p + head;
    while (p < end && *p != '{') p++;
    if (p >= end) return;
    p++;
    std::string strbuf;
    while (p < end && *p != '}') {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= end || *p == '}') break;
        if (*p != '"') break;
        p = fast_parse_string(p, end, strbuf);
        std::string key = strbuf;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p < end && *p == ':') p++;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        LogicalType t = LogicalType::VARCHAR();
        if (p < end && *p == '"') { p = fast_parse_string(p, end, strbuf); t = LogicalType::VARCHAR(); }
        else if (p < end && *p == 'n') { p += 4; }
        else if (p < end && *p == 't') { p += 4; t = LogicalType::BOOLEAN(); }
        else if (p < end && *p == 'f') { p += 5; t = LogicalType::BOOLEAN(); }
        else {
            const char *ns, *ne; bool isf;
            p = fast_parse_number_raw(p, end, ns, ne, isf);
            if (isf) t = LogicalType::DOUBLE();
            else {
                int64_t i = 0; std::from_chars(ns, ne, i);
                if (i >= INT32_MIN && i <= INT32_MAX) t = LogicalType::INTEGER();
                else t = LogicalType::BIGINT();
            }
        }
        col_names.push_back(std::move(key));
        col_types.push_back(t);
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p < end && *p == ',') p++;
    }
}

void JSONReader::DetectSchemaLight() {
    if (!column_names_.empty()) return;
    detect_ndjson_schema_from_first(path_, column_names_, column_types_);
}

void JSONReader::ReadIntoTable(DataTable &table, const std::vector<LogicalType> &types) {
    std::vector<std::vector<DataChunk>> per_thread;
    ParallelParseToPerThread(per_thread, types);
    for (auto &local : per_thread) {
        for (auto &c : local) table.Append(c);
    }
}

void JSONReader::ReadIntoChunks(std::vector<DataChunk> &chunks,
                                 const std::vector<LogicalType> &types) {
    std::vector<std::vector<DataChunk>> per_thread;
    ParallelParseToPerThread(per_thread, types);
    for (auto &local : per_thread) {
        for (auto &c : local) chunks.push_back(std::move(c));
    }
}

void JSONReader::ParallelParseToPerThread(
    std::vector<std::vector<DataChunk>> &per_thread,
    const std::vector<LogicalType> &types) {
    if (column_names_.empty()) {
        detect_ndjson_schema_from_first(path_, column_names_, column_types_);
    }

    // mmap the file — saves the ~150 ms user-space read on 165 MB files.
    const char *buf = nullptr;
    size_t sz = 0;
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open: " + path_);
    LARGE_INTEGER fsz; GetFileSizeEx(hFile, &fsz);
    sz = (size_t)fsz.QuadPart;
    HANDLE hMap = nullptr;
    void *mapview = nullptr;
    if (sz > 0) {
        hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hMap) mapview = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    }
    CloseHandle(hFile);
    if (!mapview) { if (hMap) CloseHandle(hMap); return; }
    buf = static_cast<const char *>(mapview);
#else
    int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open: " + path_);
    struct stat st; ::fstat(fd, &st); sz = (size_t)st.st_size;
    void *mapview = (sz > 0) ? ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
    ::close(fd);
    if (!mapview || mapview == MAP_FAILED) return;
    buf = static_cast<const char *>(mapview);
#endif
    if (sz == 0) return;

    const idx_t ncols = column_names_.size();
    if (ncols == 0) return;

    // Per-range parse lambda — each worker thread runs this over its byte slice
    // into a LOCAL vector<DataChunk> (no mutex contention during parse). The
    // caller then drains all workers' chunks into the DataTable serially.
    auto parse_range = [&](const char *rstart, const char *rend,
                            std::vector<DataChunk> &out_chunks) {

    // Per-column pointer cache for the active chunk.
    struct ColPtr {
        LogicalTypeId tid;
        data_ptr_t data;
        ValidityMask *validity;
        VectorStringBuffer *str_buf;
    };
    std::vector<ColPtr> cp(ncols);

    DataChunk chunk;
    chunk.Initialize(types);
    auto rebind = [&]() {
        for (idx_t c = 0; c < ncols; c++) {
            auto &vec = chunk.GetVector(c);
            cp[c].tid = types[c].id();
            cp[c].data = vec.GetData();
            cp[c].validity = &vec.GetValidity();
            cp[c].str_buf = (cp[c].tid == LogicalTypeId::VARCHAR)
                                ? &vec.GetStringBuffer() : nullptr;
        }
    };
    rebind();
    idx_t count = 0;
    std::string strbuf;
    const char *p = rstart;
    const char *end = rend;

    while (p < end) {
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        p++; // skip {
        idx_t next_expected = 0;
        while (p < end && *p != '}') {
            // Skip whitespace.
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (p >= end || *p == '}') break;
            if (*p != '"') { while (p < end && *p != '\n') p++; break; }
            p++; // opening "
            const char *key_start = p;
            while (p < end && *p != '"') p++;
            size_t key_len = (size_t)(p - key_start);
            if (p < end) p++; // closing "

            // Match key to a column (positional with linear fallback).
            idx_t col = static_cast<idx_t>(-1);
            if (next_expected < ncols &&
                column_names_[next_expected].size() == key_len &&
                std::memcmp(column_names_[next_expected].data(),
                             key_start, key_len) == 0) {
                col = next_expected;
            } else {
                for (idx_t i = 0; i < ncols; i++) {
                    if (column_names_[i].size() == key_len &&
                        std::memcmp(column_names_[i].data(), key_start, key_len) == 0) {
                        col = i; break;
                    }
                }
            }

            // Skip ws + ':' + ws.
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p < end && *p == ':') p++;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;

            // Parse value and write directly to the chunk column.
            if (p < end && *p == '"') {
                p++;
                const char *vs = p;
                // Fast path: no-escape scan; fall back to general parse if
                // we hit a backslash.
                while (p < end && *p != '"' && *p != '\\') p++;
                if (p < end && *p == '\\') {
                    // Copy so-far into strbuf then continue with escapes.
                    strbuf.assign(vs, p - vs);
                    while (p < end && *p != '"') {
                        if (*p == '\\' && p + 1 < end) {
                            p++;
                            switch (*p) {
                            case '"':  strbuf.push_back('"'); break;
                            case '\\': strbuf.push_back('\\'); break;
                            case '/':  strbuf.push_back('/'); break;
                            case 'n':  strbuf.push_back('\n'); break;
                            case 't':  strbuf.push_back('\t'); break;
                            case 'r':  strbuf.push_back('\r'); break;
                            case 'b':  strbuf.push_back('\b'); break;
                            case 'f':  strbuf.push_back('\f'); break;
                            default:   strbuf.push_back(*p); break;
                            }
                            p++;
                        } else { strbuf.push_back(*p++); }
                    }
                    vs = strbuf.data();
                    key_len = strbuf.size();
                    if (col != static_cast<idx_t>(-1) && cp[col].tid == LogicalTypeId::VARCHAR) {
                        auto *arr = reinterpret_cast<string_t *>(cp[col].data);
                        if (strbuf.size() <= string_t::INLINE_LENGTH) {
                            arr[count] = string_t(strbuf.data(), (uint32_t)strbuf.size());
                        } else {
                            const char *heap = cp[col].str_buf->AddString(strbuf);
                            arr[count] = string_t(heap, (uint32_t)strbuf.size());
                        }
                    }
                    if (p < end) p++; // closing "
                } else {
                    size_t vlen = (size_t)(p - vs);
                    if (p < end) p++; // closing "
                    if (col != static_cast<idx_t>(-1) && cp[col].tid == LogicalTypeId::VARCHAR) {
                        auto *arr = reinterpret_cast<string_t *>(cp[col].data);
                        if (vlen <= string_t::INLINE_LENGTH) {
                            arr[count] = string_t(vs, (uint32_t)vlen);
                        } else {
                            const char *heap = cp[col].str_buf->AddString(vs, vlen);
                            arr[count] = string_t(heap, (uint32_t)vlen);
                        }
                    }
                }
            } else if (p < end && *p == 'n') {
                p += 4;
                if (col != static_cast<idx_t>(-1)) cp[col].validity->SetInvalid(count);
            } else if (p < end && *p == 't') {
                p += 4;
                if (col != static_cast<idx_t>(-1) && cp[col].tid == LogicalTypeId::BOOLEAN) {
                    reinterpret_cast<bool *>(cp[col].data)[count] = true;
                }
            } else if (p < end && *p == 'f') {
                p += 5;
                if (col != static_cast<idx_t>(-1) && cp[col].tid == LogicalTypeId::BOOLEAN) {
                    reinterpret_cast<bool *>(cp[col].data)[count] = false;
                }
            } else {
                const char *ns = p;
                if (p < end && *p == '-') p++;
                while (p < end && *p >= '0' && *p <= '9') p++;
                bool is_float = false;
                if (p < end && *p == '.') { is_float = true; p++;
                    while (p < end && *p >= '0' && *p <= '9') p++; }
                if (p < end && (*p == 'e' || *p == 'E')) { is_float = true; p++;
                    if (p < end && (*p == '+' || *p == '-')) p++;
                    while (p < end && *p >= '0' && *p <= '9') p++; }
                const char *ne = p;
                if (col != static_cast<idx_t>(-1)) {
                    switch (cp[col].tid) {
                    case LogicalTypeId::DOUBLE: {
                        double d = 0; std::from_chars(ns, ne, d);
                        reinterpret_cast<double *>(cp[col].data)[count] = d;
                        break;
                    }
                    case LogicalTypeId::FLOAT: {
                        double d = 0; std::from_chars(ns, ne, d);
                        reinterpret_cast<float *>(cp[col].data)[count] = (float)d;
                        break;
                    }
                    case LogicalTypeId::BIGINT: {
                        if (is_float) { double d = 0; std::from_chars(ns, ne, d);
                            reinterpret_cast<int64_t *>(cp[col].data)[count] = (int64_t)d; }
                        else { int64_t i = 0; std::from_chars(ns, ne, i);
                            reinterpret_cast<int64_t *>(cp[col].data)[count] = i; }
                        break;
                    }
                    case LogicalTypeId::INTEGER: {
                        if (is_float) { double d = 0; std::from_chars(ns, ne, d);
                            reinterpret_cast<int32_t *>(cp[col].data)[count] = (int32_t)d; }
                        else { int64_t i = 0; std::from_chars(ns, ne, i);
                            reinterpret_cast<int32_t *>(cp[col].data)[count] = (int32_t)i; }
                        break;
                    }
                    case LogicalTypeId::VARCHAR: {
                        auto *arr = reinterpret_cast<string_t *>(cp[col].data);
                        size_t n = (size_t)(ne - ns);
                        if (n <= string_t::INLINE_LENGTH) {
                            arr[count] = string_t(ns, (uint32_t)n);
                        } else {
                            const char *heap = cp[col].str_buf->AddString(ns, n);
                            arr[count] = string_t(heap, (uint32_t)n);
                        }
                        break;
                    }
                    default: break;
                    }
                }
            }
            if (col != static_cast<idx_t>(-1)) next_expected = col + 1;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p < end && *p == ',') p++;
        }
        if (p < end && *p == '}') p++;
        count++;
        if (count == VECTOR_SIZE) {
            chunk.SetCardinality(count);
            out_chunks.push_back(std::move(chunk));
            chunk = DataChunk{};
            chunk.Initialize(types);
            rebind();
            count = 0;
        }
    }
    if (count > 0) {
        chunk.SetCardinality(count);
        out_chunks.push_back(std::move(chunk));
    }
    }; // end parse_range

    // Split the buffer into line-aligned byte ranges and parse in parallel.
    // For small files stay serial — thread spawn overhead would dominate.
    const char *data = buf;
    size_t total = sz;
    unsigned int nt = std::thread::hardware_concurrency();
    if (nt == 0) nt = 4;
    if (nt > 8) nt = 8;
    if (total < 2 * 1024 * 1024) nt = 1;

    // Release the mmap at scope exit.
    struct MmapGuard {
        void *view = nullptr;
        size_t len = 0;
#ifdef _WIN32
        void *hmap = nullptr;
#endif
        ~MmapGuard() {
#ifdef _WIN32
            if (view) UnmapViewOfFile(view);
            if (hmap) CloseHandle((HANDLE)hmap);
#else
            if (view && view != MAP_FAILED) ::munmap(view, len);
#endif
        }
    } g;
    g.view = const_cast<char *>(buf);
    g.len = sz;
#ifdef _WIN32
    g.hmap = hMap;
#endif

    if (nt == 1) {
        per_thread.resize(1);
        parse_range(data, data + total, per_thread[0]);
        return;
    }

    std::vector<std::pair<size_t, size_t>> ranges(nt);
    for (unsigned int i = 0; i < nt; i++) {
        size_t start = (total * (size_t)i) / nt;
        size_t stop  = (total * (size_t)(i + 1)) / nt;
        if (i > 0) { // snap start to next newline boundary
            while (start < total && data[start] != '\n') start++;
            if (start < total) start++;
        }
        while (stop < total && data[stop] != '\n') stop++;
        if (stop < total) stop++;
        ranges[i] = {start, stop};
    }
    std::vector<std::thread> workers;
    per_thread.clear();
    per_thread.resize(nt);
    workers.reserve(nt);
    for (unsigned int i = 0; i < nt; i++) {
        auto [s, e] = ranges[i];
        workers.emplace_back([&, s, e, i]() {
            parse_range(data + s, data + e, per_thread[i]);
        });
    }
    for (auto &t : workers) t.join();
}

std::vector<std::vector<Value>> JSONReader::ReadAll() {
    if (!parsed_) DetectSchema();

    // Fast NDJSON path: rows_ already populated; avoid the records_→rows copy.
    if (!rows_.empty()) return std::move(rows_);

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

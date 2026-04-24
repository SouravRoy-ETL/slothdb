// SlothDB Emscripten bindings for the try.slothdb.org playground.
//
// Exposes a tiny embind surface on top of the stable C API:
//   - openDatabase()      -> creates an in-memory DB + connection once
//   - runQuery(sql)       -> returns a JSON string: {columns, rows, error, ms}
//   - version()           -> slothdb_version()
//
// JSON is hand-rolled (no external dep) so the wasm binary stays small.

#include "slothdb/api/slothdb.h"

#include <emscripten/bind.h>
#include <chrono>
#include <cstdint>
#include <string>

namespace {

slothdb_database *g_db = nullptr;
slothdb_connection *g_conn = nullptr;

void EnsureOpen() {
    if (g_db) return;
    slothdb_open("", &g_db);
    slothdb_connect(g_db, &g_conn);
}

// Append `s` as a JSON string literal. Escapes ", \, \n, \r, \t and control chars.
void AppendJSONString(std::string &out, const char *s) {
    out.push_back('"');
    if (!s) { out.push_back('"'); return; }
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back((char)c);
            }
        }
    }
    out.push_back('"');
}

std::string OpenDatabase() {
    EnsureOpen();
    return "ok";
}

std::string RunQuery(std::string sql) {
    EnsureOpen();

    auto t0 = std::chrono::steady_clock::now();
    slothdb_result *result = nullptr;
    auto status = slothdb_query(g_conn, sql.c_str(), &result);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::string out;
    out.reserve(256);

    if (status != SLOTHDB_OK) {
        out += "{\"error\":";
        AppendJSONString(out, slothdb_result_error(result));
        out += ",\"ms\":";
        out += std::to_string(ms);
        out += "}";
        slothdb_free_result(result);
        return out;
    }

    uint64_t cols = slothdb_column_count(result);
    uint64_t rows = slothdb_row_count(result);

    out += "{\"columns\":[";
    for (uint64_t c = 0; c < cols; c++) {
        if (c > 0) out.push_back(',');
        AppendJSONString(out, slothdb_column_name(result, c));
    }
    out += "],\"rows\":[";
    for (uint64_t r = 0; r < rows; r++) {
        if (r > 0) out.push_back(',');
        out.push_back('[');
        for (uint64_t c = 0; c < cols; c++) {
            if (c > 0) out.push_back(',');
            if (slothdb_value_is_null(result, r, c)) {
                out += "null";
            } else {
                AppendJSONString(out, slothdb_value_varchar(result, r, c));
            }
        }
        out.push_back(']');
    }
    out += "],\"ms\":";
    out += std::to_string(ms);
    out += ",\"rowCount\":";
    out += std::to_string(rows);
    out += "}";

    slothdb_free_result(result);
    return out;
}

std::string Version() {
    return slothdb_version();
}

} // namespace

EMSCRIPTEN_BINDINGS(slothdb) {
    emscripten::function("openDatabase", &OpenDatabase);
    emscripten::function("runQuery", &RunQuery);
    emscripten::function("version", &Version);
}

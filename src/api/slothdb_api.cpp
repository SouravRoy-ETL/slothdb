#include "slothdb/api/slothdb.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/catalog/catalog.hpp"
#include "slothdb/catalog/schema_catalog_entry.hpp"
#include "slothdb/catalog/table_catalog_entry.hpp"
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace slothdb;

struct slothdb_database {
    std::unique_ptr<Database> db;
};

struct slothdb_connection {
    std::unique_ptr<Connection> conn;
};

struct slothdb_result {
    QueryResult result;
    std::string error;
    // Cache for string values (keeps pointers alive until result is freed).
    std::vector<std::string> string_cache;
    // Lazy typed-batch buffers, one per column. Built on first call to
    // slothdb_column_int32/int64/double_buffer for that column and
    // reused across calls (and across re-fetches by the same caller).
    struct ColumnBuf {
        std::vector<int32_t>  i32;
        std::vector<int64_t>  i64;
        std::vector<double>   f64;
        std::vector<uint8_t>  validity;
        bool has_nulls = false;
        std::vector<uint64_t> str_offsets;   // row_count+1 entries
        std::vector<char>     str_blob;
        bool i32_built = false;
        bool i64_built = false;
        bool f64_built = false;
        bool str_built = false;
    };
    std::vector<ColumnBuf> col_bufs;
};

extern "C" {

slothdb_status slothdb_open(const char *path, slothdb_database **out_db) {
    if (!out_db) return SLOTHDB_INVALID;
    try {
        auto *db = new slothdb_database();
        if (path && strlen(path) > 0) {
            db->db = std::make_unique<Database>(std::string(path));
        } else {
            db->db = std::make_unique<Database>();
        }
        *out_db = db;
        return SLOTHDB_OK;
    } catch (...) {
        return SLOTHDB_ERROR;
    }
}

void slothdb_close(slothdb_database *db) {
    delete db;
}

slothdb_status slothdb_connect(slothdb_database *db, slothdb_connection **out_conn) {
    if (!db || !out_conn) return SLOTHDB_INVALID;
    try {
        auto *conn = new slothdb_connection();
        conn->conn = std::make_unique<Connection>(*db->db);
        *out_conn = conn;
        return SLOTHDB_OK;
    } catch (...) {
        return SLOTHDB_ERROR;
    }
}

void slothdb_disconnect(slothdb_connection *conn) {
    delete conn;
}

slothdb_status slothdb_query(slothdb_connection *conn, const char *sql,
                              slothdb_result **out_result) {
    if (!conn || !sql || !out_result) return SLOTHDB_INVALID;
    auto *res = new slothdb_result();
    try {
        res->result = conn->conn->Query(sql);
        *out_result = res;
        return SLOTHDB_OK;
    } catch (const std::exception &e) {
        res->error = e.what();
        *out_result = res;
        return SLOTHDB_ERROR;
    }
}

const char *slothdb_result_error(slothdb_result *result) {
    if (!result) return "";
    return result->error.c_str();
}

uint64_t slothdb_column_count(slothdb_result *result) {
    if (!result) return 0;
    return result->result.ColumnCount();
}

uint64_t slothdb_row_count(slothdb_result *result) {
    if (!result) return 0;
    return result->result.RowCount();
}

const char *slothdb_column_name(slothdb_result *result, uint64_t col) {
    if (!result || col >= result->result.ColumnCount()) return "";
    return result->result.column_names[col].c_str();
}

slothdb_type slothdb_column_type(slothdb_result *result, uint64_t col) {
    if (!result || col >= result->result.ColumnCount()) return SLOTHDB_TYPE_INVALID;
    auto id = result->result.column_types[col].id();
    switch (id) {
    case LogicalTypeId::BOOLEAN: return SLOTHDB_TYPE_BOOLEAN;
    case LogicalTypeId::INTEGER: return SLOTHDB_TYPE_INTEGER;
    case LogicalTypeId::BIGINT: return SLOTHDB_TYPE_BIGINT;
    case LogicalTypeId::FLOAT: return SLOTHDB_TYPE_FLOAT;
    case LogicalTypeId::DOUBLE: return SLOTHDB_TYPE_DOUBLE;
    case LogicalTypeId::VARCHAR: return SLOTHDB_TYPE_VARCHAR;
    default: return SLOTHDB_TYPE_INVALID;
    }
}

int slothdb_value_is_null(slothdb_result *result, uint64_t row, uint64_t col) {
    if (!result || row >= result->result.RowCount()) return 1;
    return result->result.GetValue(row, col).IsNull() ? 1 : 0;
}

int32_t slothdb_value_int32(slothdb_result *result, uint64_t row, uint64_t col) {
    if (!result || row >= result->result.RowCount()) return 0;
    auto &val = result->result.GetValue(row, col);
    if (val.IsNull()) return 0;
    return val.GetValue<int32_t>();
}

int64_t slothdb_value_int64(slothdb_result *result, uint64_t row, uint64_t col) {
    if (!result || row >= result->result.RowCount()) return 0;
    auto &val = result->result.GetValue(row, col);
    if (val.IsNull()) return 0;
    if (val.type().id() == LogicalTypeId::INTEGER) return val.GetValue<int32_t>();
    return val.GetValue<int64_t>();
}

double slothdb_value_double(slothdb_result *result, uint64_t row, uint64_t col) {
    if (!result || row >= result->result.RowCount()) return 0.0;
    auto &val = result->result.GetValue(row, col);
    if (val.IsNull()) return 0.0;
    return val.GetValue<double>();
}

const char *slothdb_value_varchar(slothdb_result *result, uint64_t row, uint64_t col) {
    if (!result || row >= result->result.RowCount()) return "";
    auto &val = result->result.GetValue(row, col);
    if (val.IsNull()) return "NULL";
    result->string_cache.push_back(val.ToString());
    return result->string_cache.back().c_str();
}

void slothdb_free_result(slothdb_result *result) {
    delete result;
}

// ============================================================================
// Typed batch fetch — populate per-column buffers on demand and return raw
// pointers. One C call per column instead of two per cell — drops 60M
// ctypes calls to ~6 for a 10M x 3 query.
// ============================================================================

static slothdb_result::ColumnBuf *EnsureColBuf(slothdb_result *r, uint64_t col) {
    if (!r) return nullptr;
    if (col >= r->result.ColumnCount()) return nullptr;
    if (r->col_bufs.size() != r->result.ColumnCount()) {
        r->col_bufs.resize(r->result.ColumnCount());
    }
    return &r->col_bufs[col];
}

static void BuildValidity(slothdb_result *r, uint64_t col,
                          slothdb_result::ColumnBuf &buf) {
    if (!buf.validity.empty()) return;
    auto n = r->result.RowCount();
    buf.validity.assign(n, 1);
    bool any_null = false;
    for (uint64_t i = 0; i < n; i++) {
        if (r->result.GetValue(i, col).IsNull()) {
            buf.validity[i] = 0;
            any_null = true;
        }
    }
    buf.has_nulls = any_null;
}

const int32_t *slothdb_column_int32_buffer(slothdb_result *result, uint64_t col) {
    auto *buf = EnsureColBuf(result, col);
    if (!buf) return nullptr;
    if (slothdb_column_type(result, col) != SLOTHDB_TYPE_INTEGER) return nullptr;
    if (buf->i32_built) return buf->i32.data();
    auto n = result->result.RowCount();
    buf->i32.resize(n);
    for (uint64_t i = 0; i < n; i++) {
        auto &v = result->result.GetValue(i, col);
        buf->i32[i] = v.IsNull() ? 0 : v.GetValue<int32_t>();
    }
    BuildValidity(result, col, *buf);
    buf->i32_built = true;
    return buf->i32.data();
}

const int64_t *slothdb_column_int64_buffer(slothdb_result *result, uint64_t col) {
    auto *buf = EnsureColBuf(result, col);
    if (!buf) return nullptr;
    auto t = slothdb_column_type(result, col);
    if (t != SLOTHDB_TYPE_BIGINT && t != SLOTHDB_TYPE_INTEGER) return nullptr;
    if (buf->i64_built) return buf->i64.data();
    auto n = result->result.RowCount();
    buf->i64.resize(n);
    if (t == SLOTHDB_TYPE_BIGINT) {
        for (uint64_t i = 0; i < n; i++) {
            auto &v = result->result.GetValue(i, col);
            buf->i64[i] = v.IsNull() ? 0 : v.GetValue<int64_t>();
        }
    } else {
        for (uint64_t i = 0; i < n; i++) {
            auto &v = result->result.GetValue(i, col);
            buf->i64[i] = v.IsNull() ? 0 : (int64_t)v.GetValue<int32_t>();
        }
    }
    BuildValidity(result, col, *buf);
    buf->i64_built = true;
    return buf->i64.data();
}

const double *slothdb_column_double_buffer(slothdb_result *result, uint64_t col) {
    auto *buf = EnsureColBuf(result, col);
    if (!buf) return nullptr;
    auto t = slothdb_column_type(result, col);
    if (t != SLOTHDB_TYPE_DOUBLE && t != SLOTHDB_TYPE_FLOAT) return nullptr;
    if (buf->f64_built) return buf->f64.data();
    auto n = result->result.RowCount();
    buf->f64.resize(n);
    for (uint64_t i = 0; i < n; i++) {
        auto &v = result->result.GetValue(i, col);
        buf->f64[i] = v.IsNull() ? 0.0
            : (t == SLOTHDB_TYPE_DOUBLE ? v.GetValue<double>()
                                        : (double)v.GetValue<float>());
    }
    BuildValidity(result, col, *buf);
    buf->f64_built = true;
    return buf->f64.data();
}

const uint8_t *slothdb_column_validity_buffer(slothdb_result *result, uint64_t col) {
    auto *buf = EnsureColBuf(result, col);
    if (!buf) return nullptr;
    BuildValidity(result, col, *buf);
    return buf->has_nulls ? buf->validity.data() : nullptr;
}

int slothdb_column_varchar_buffer(slothdb_result *result, uint64_t col,
                                   const uint64_t **out_offsets,
                                   const char **out_blob) {
    auto *buf = EnsureColBuf(result, col);
    if (!buf) return -1;
    if (slothdb_column_type(result, col) != SLOTHDB_TYPE_VARCHAR) return -1;
    if (!buf->str_built) {
        auto n = result->result.RowCount();
        buf->str_offsets.assign(n + 1, 0);
        // First pass: compute total bytes and per-row lengths.
        size_t total = 0;
        std::vector<std::string> tmp(n);
        for (uint64_t i = 0; i < n; i++) {
            auto &v = result->result.GetValue(i, col);
            if (!v.IsNull()) tmp[i] = v.ToString();
            total += tmp[i].size();
            buf->str_offsets[i + 1] = total;
        }
        buf->str_blob.resize(total);
        // Second pass: copy.
        for (uint64_t i = 0; i < n; i++) {
            auto off = buf->str_offsets[i];
            std::memcpy(buf->str_blob.data() + off, tmp[i].data(), tmp[i].size());
        }
        BuildValidity(result, col, *buf);
        buf->str_built = true;
    }
    if (out_offsets) *out_offsets = buf->str_offsets.data();
    if (out_blob) *out_blob = buf->str_blob.empty() ? "" : buf->str_blob.data();
    return 0;
}

// Thread-local cache for the catalog-introspection C API's returned
// strings. The C API contract is "pointer valid until next catalog
// mutation on this connection". We store names in a thread-local arena
// so the pointer outlives the GetTable()/GetColumns() call without
// requiring the caller to free anything. Simple and good enough -
// `.ask` etc. pull a few dozen names at a time.
namespace {
std::string &introspection_slot(size_t idx) {
    static thread_local std::vector<std::string> arena;
    if (idx >= arena.size()) arena.resize(idx + 16);
    return arena[idx];
}
}

uint64_t slothdb_table_count(slothdb_connection *conn) {
    if (!conn || !conn->conn) return 0;
    try {
        auto &schema = conn->conn->GetCatalog().GetSchema();
        return static_cast<uint64_t>(schema.GetTableNames().size());
    } catch (...) { return 0; }
}

const char *slothdb_table_name(slothdb_connection *conn, uint64_t i) {
    if (!conn || !conn->conn) return "";
    try {
        auto &schema = conn->conn->GetCatalog().GetSchema();
        auto names = schema.GetTableNames();
        if (i >= names.size()) return "";
        auto &slot = introspection_slot(0);
        slot = names[static_cast<size_t>(i)];
        return slot.c_str();
    } catch (...) { return ""; }
}

uint64_t slothdb_table_column_count(slothdb_connection *conn, uint64_t ti) {
    if (!conn || !conn->conn) return 0;
    try {
        auto &schema = conn->conn->GetCatalog().GetSchema();
        auto names = schema.GetTableNames();
        if (ti >= names.size()) return 0;
        auto *tbl = schema.GetTable(names[static_cast<size_t>(ti)]);
        return tbl ? tbl->ColumnCount() : 0;
    } catch (...) { return 0; }
}

const char *slothdb_table_column_name(slothdb_connection *conn,
                                       uint64_t ti, uint64_t ci) {
    if (!conn || !conn->conn) return "";
    try {
        auto &schema = conn->conn->GetCatalog().GetSchema();
        auto names = schema.GetTableNames();
        if (ti >= names.size()) return "";
        auto *tbl = schema.GetTable(names[static_cast<size_t>(ti)]);
        if (!tbl || ci >= tbl->ColumnCount()) return "";
        auto &slot = introspection_slot(1);
        slot = tbl->GetColumns()[static_cast<size_t>(ci)].name;
        return slot.c_str();
    } catch (...) { return ""; }
}

const char *slothdb_table_column_type(slothdb_connection *conn,
                                       uint64_t ti, uint64_t ci) {
    if (!conn || !conn->conn) return "";
    try {
        auto &schema = conn->conn->GetCatalog().GetSchema();
        auto names = schema.GetTableNames();
        if (ti >= names.size()) return "";
        auto *tbl = schema.GetTable(names[static_cast<size_t>(ti)]);
        if (!tbl || ci >= tbl->ColumnCount()) return "";
        auto &slot = introspection_slot(2);
        slot = tbl->GetColumns()[static_cast<size_t>(ci)].type.ToString();
        return slot.c_str();
    } catch (...) { return ""; }
}

const char *slothdb_version(void) {
    return "0.2.0";
}

} /* extern "C" */

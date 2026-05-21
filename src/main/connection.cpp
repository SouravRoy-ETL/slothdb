#include "slothdb/main/connection.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/parser/parser.hpp"
#include "slothdb/binder/binder.hpp"
#include "slothdb/planner/planner.hpp"
#include "slothdb/execution/physical_planner.hpp"
#include "slothdb/execution/expression_executor.hpp"
#include "slothdb/optimizer/optimizer.hpp"
#include "slothdb/storage/csv_reader.hpp"
#include "slothdb/storage/fast_csv_reader.hpp"
#include "slothdb/storage/json_reader.hpp"
#include "slothdb/storage/parquet.hpp"
#ifndef SLOTHDB_EDGE
#include "slothdb/storage/arrow_ipc.hpp"
#include "slothdb/storage/avro_reader.hpp"
#include "slothdb/storage/excel_reader.hpp"
#include "slothdb/storage/sqlite_scanner.hpp"
#endif
#include "slothdb/storage/http_client.hpp"
#include "slothdb/storage/hive_partition.hpp"
#include "slothdb/common/string_util.hpp"
#include "slothdb/common/file_glob.hpp"
#include <sstream>
#include <unordered_set>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace slothdb {

// Bulk-load rows into a DataTable using chunked appends (VECTOR_SIZE rows per chunk).
// This avoids creating one DataChunk per row, which is the primary bottleneck.
static void BulkLoadRows(DataTable &storage, const std::vector<LogicalType> &types,
                          const std::vector<std::vector<Value>> &rows) {
    const idx_t chunk_size = VECTOR_SIZE; // 2048 rows per chunk
    idx_t total = static_cast<idx_t>(rows.size());
    idx_t num_cols = static_cast<idx_t>(types.size());

    for (idx_t offset = 0; offset < total; offset += chunk_size) {
        idx_t count = std::min(chunk_size, total - offset);
        DataChunk chunk;
        chunk.Initialize(types);
        for (idx_t r = 0; r < count; r++) {
            auto &row = rows[offset + r];
            for (idx_t c = 0; c < num_cols && c < static_cast<idx_t>(row.size()); c++) {
                chunk.SetValue(c, r, row[c]);
            }
        }
        chunk.SetCardinality(count);
        storage.Append(chunk);
    }
}

// SQL-standard wider common type for two branches of a UNION / INTERSECT /
// EXCEPT chain (per-column). The pairwise reduction over a chain produces
// the type each branch's values must coerce to before set-op dedup or
// storage. Falls back to VARCHAR (universal lossless string) on any
// unknown combination; the caller is responsible for coercing values.
static LogicalType ComputeSetOpCommonType(const LogicalType &a, const LogicalType &b) {
    if (a.id() == b.id()) return a;
    if (a.id() == LogicalTypeId::INVALID || a.id() == LogicalTypeId::SQLNULL) return b;
    if (b.id() == LogicalTypeId::INVALID || b.id() == LogicalTypeId::SQLNULL) return a;

    auto is_signed_int = [](LogicalTypeId id) {
        return id == LogicalTypeId::TINYINT || id == LogicalTypeId::SMALLINT ||
               id == LogicalTypeId::INTEGER || id == LogicalTypeId::BIGINT;
    };
    auto is_unsigned_int = [](LogicalTypeId id) {
        return id == LogicalTypeId::UTINYINT || id == LogicalTypeId::USMALLINT ||
               id == LogicalTypeId::UINTEGER || id == LogicalTypeId::UBIGINT;
    };
    auto is_float = [](LogicalTypeId id) {
        return id == LogicalTypeId::FLOAT || id == LogicalTypeId::DOUBLE;
    };
    auto width = [&](LogicalTypeId id) -> int {
        switch (id) {
            case LogicalTypeId::BOOLEAN:   return 1;
            case LogicalTypeId::TINYINT:   case LogicalTypeId::UTINYINT:   return 8;
            case LogicalTypeId::SMALLINT:  case LogicalTypeId::USMALLINT:  return 16;
            case LogicalTypeId::INTEGER:   case LogicalTypeId::UINTEGER:   return 32;
            case LogicalTypeId::BIGINT:    case LogicalTypeId::UBIGINT:    return 64;
            case LogicalTypeId::FLOAT:     return 32;
            case LogicalTypeId::DOUBLE:    return 64;
            case LogicalTypeId::HUGEINT:   return 128;
            default: return 0;
        }
    };

    // Mixed float + int → DOUBLE if either is DOUBLE or width≥32 int, else FLOAT.
    if (is_float(a.id()) || is_float(b.id())) {
        if (a.id() == LogicalTypeId::DOUBLE || b.id() == LogicalTypeId::DOUBLE ||
            width(a.id()) >= 32 || width(b.id()) >= 32) {
            return LogicalType::DOUBLE();
        }
        return LogicalType(LogicalTypeId::FLOAT);
    }

    // Mixed signed + unsigned int → next-up signed (UBIGINT vs INTEGER stays
    // at BIGINT — a true UBIGINT span needs HUGEINT but those queries are
    // rare and the current codebase doesn't carry HUGEINT well; clamp at
    // BIGINT and accept the rare overflow rather than escalate to a type
    // the rest of the engine can't handle).
    if ((is_signed_int(a.id()) && is_unsigned_int(b.id())) ||
        (is_unsigned_int(a.id()) && is_signed_int(b.id()))) {
        int w = std::max(width(a.id()), width(b.id()));
        if (w <= 8)  return LogicalType(LogicalTypeId::SMALLINT);
        if (w <= 16) return LogicalType(LogicalTypeId::INTEGER);
        return LogicalType::BIGINT();
    }

    // Same family, different width.
    if (is_signed_int(a.id()) && is_signed_int(b.id())) {
        return (width(a.id()) >= width(b.id())) ? a : b;
    }
    if (is_unsigned_int(a.id()) && is_unsigned_int(b.id())) {
        return (width(a.id()) >= width(b.id())) ? a : b;
    }

    // Anything else (DATE/TIMESTAMP/BLOB/LIST mixed with non-matching) →
    // VARCHAR is the only universal target the engine can carry.
    return LogicalType(LogicalTypeId::VARCHAR);
}

// Coerce a single Value to the target type. NULLs are passed through with
// the target type tag. Falls back to VARCHAR via ToString() on any path
// not explicitly handled — same fallback policy as ExecuteCast.
static Value CoerceValueForSetOp(const Value &v, const LogicalType &target) {
    if (v.IsNull()) return Value();
    if (v.type().id() == target.id()) return v;

    // Read the source as int64 / double / string in a type-dispatched way.
    auto src_id = v.type().id();
    auto src_as_int64 = [&]() -> int64_t {
        switch (src_id) {
            case LogicalTypeId::BOOLEAN:  return v.GetValue<bool>() ? 1 : 0;
            case LogicalTypeId::TINYINT:  return v.GetValue<int8_t>();
            case LogicalTypeId::SMALLINT: return v.GetValue<int16_t>();
            case LogicalTypeId::INTEGER:  return v.GetValue<int32_t>();
            case LogicalTypeId::BIGINT:   return v.GetValue<int64_t>();
            case LogicalTypeId::UTINYINT: return v.GetValue<uint8_t>();
            case LogicalTypeId::USMALLINT:return v.GetValue<uint16_t>();
            case LogicalTypeId::UINTEGER: return v.GetValue<uint32_t>();
            case LogicalTypeId::UBIGINT:  return static_cast<int64_t>(v.GetValue<uint64_t>());
            case LogicalTypeId::FLOAT:    return static_cast<int64_t>(v.GetValue<float>());
            case LogicalTypeId::DOUBLE:   return static_cast<int64_t>(v.GetValue<double>());
            default: return 0;
        }
    };
    auto src_as_double = [&]() -> double {
        switch (src_id) {
            case LogicalTypeId::BOOLEAN:  return v.GetValue<bool>() ? 1.0 : 0.0;
            case LogicalTypeId::TINYINT:  return v.GetValue<int8_t>();
            case LogicalTypeId::SMALLINT: return v.GetValue<int16_t>();
            case LogicalTypeId::INTEGER:  return v.GetValue<int32_t>();
            case LogicalTypeId::BIGINT:   return static_cast<double>(v.GetValue<int64_t>());
            case LogicalTypeId::UTINYINT: return v.GetValue<uint8_t>();
            case LogicalTypeId::USMALLINT:return v.GetValue<uint16_t>();
            case LogicalTypeId::UINTEGER: return v.GetValue<uint32_t>();
            case LogicalTypeId::UBIGINT:  return static_cast<double>(v.GetValue<uint64_t>());
            case LogicalTypeId::FLOAT:    return v.GetValue<float>();
            case LogicalTypeId::DOUBLE:   return v.GetValue<double>();
            default: return 0.0;
        }
    };

    switch (target.id()) {
        case LogicalTypeId::TINYINT:  return Value::TINYINT(static_cast<int8_t>(src_as_int64()));
        case LogicalTypeId::SMALLINT: return Value::SMALLINT(static_cast<int16_t>(src_as_int64()));
        case LogicalTypeId::INTEGER:  return Value::INTEGER(static_cast<int32_t>(src_as_int64()));
        case LogicalTypeId::BIGINT:   return Value::BIGINT(src_as_int64());
        case LogicalTypeId::UTINYINT: return Value::UTINYINT(static_cast<uint8_t>(src_as_int64()));
        case LogicalTypeId::USMALLINT:return Value::USMALLINT(static_cast<uint16_t>(src_as_int64()));
        case LogicalTypeId::UINTEGER: return Value::UINTEGER(static_cast<uint32_t>(src_as_int64()));
        case LogicalTypeId::UBIGINT:  return Value::UBIGINT(static_cast<uint64_t>(src_as_int64()));
        case LogicalTypeId::FLOAT:    return Value::FLOAT(static_cast<float>(src_as_double()));
        case LogicalTypeId::DOUBLE:   return Value::DOUBLE(src_as_double());
        case LogicalTypeId::VARCHAR:  return Value::VARCHAR(v.ToString());
        default: return v;  // unknown target — pass through
    }
}

// Hash + equality on a row of Values, used for UNION / INTERSECT / EXCEPT
// dedup. NULL == NULL per set-op semantics (Value::operator== already
// handles this correctly). Type-aware: INT 1 and VARCHAR "1" don't
// collide, unlike the prior string-concatenation key.
struct SetOpRowHash {
    size_t operator()(const std::vector<Value> &row) const noexcept {
        size_t h = row.size();
        for (auto &v : row) {
            size_t vh;
            if (v.IsNull()) {
                vh = 0xDEAD9E37;
            } else {
                vh = std::hash<std::string>{}(v.ToString());
                vh ^= static_cast<size_t>(v.type().id()) * 0x9e3779b9u;
            }
            h ^= vh + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};
struct SetOpRowEqual {
    bool operator()(const std::vector<Value> &a, const std::vector<Value> &b) const noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (!(a[i] == b[i])) return false;
        }
        return true;
    }
};
using SetOpRowSet = std::unordered_set<std::vector<Value>, SetOpRowHash, SetOpRowEqual>;

// Apply union-level ORDER BY / LIMIT / OFFSET to a row vector.
//
// The parser attaches ORDER BY / LIMIT / OFFSET that appear after a UNION /
// INTERSECT / EXCEPT to the rightmost (leaf) SelectStatement of the chain,
// because it walks left-to-right and consumes those clauses as part of the
// final ParseSelectStatement call. Per SQL standard those clauses apply to
// the entire set-op result, not to the leaf branch alone. The connection
// layer stashes them off the leaf, executes the chain unsorted, then calls
// this to sort + trim the combined rows.
//
// Sort key resolution is limited to positional integer literals
// (`ORDER BY 1 DESC`) and bare column-name refs (`ORDER BY a`) since the
// chain-combined row vector isn't a fully-bound DataChunk and a generic
// expression evaluator would need to be re-bound against the result-set
// schema. Items that don't resolve are skipped — the rows stay in
// chain-walk order for that key, which matches what slothdb did before
// this fix.
//
// LIMIT / OFFSET must be integer literals. Non-literal expressions are
// silently ignored. This is the same restriction sqllogictest's corpus
// uses everywhere, and matches the parser's expectations.
static void ApplyUnionOrderLimit(
        std::vector<std::vector<Value>> &rows,
        const std::vector<OrderByItem> &order_by,
        const ParsedExprPtr &limit_expr,
        const ParsedExprPtr &offset_expr,
        const std::vector<std::string> &col_names) {
    if (!order_by.empty()) {
        struct Key { int col_idx; bool ascending; bool nulls_first; };
        std::vector<Key> keys;
        keys.reserve(order_by.size());
        for (auto &item : order_by) {
            Key k{ -1, item.ascending, item.nulls_first };
            auto &e = *item.expression;
            if (e.GetExpressionType() == ExpressionType::CONSTANT) {
                auto &c = static_cast<const ConstantExpression &>(e);
                if (c.literal_type == TokenType::INTEGER_LITERAL) {
                    try {
                        int v = std::stoi(c.value);
                        if (v >= 1 && v <= static_cast<int>(col_names.size()))
                            k.col_idx = v - 1;
                    } catch (...) {}
                }
            } else if (e.GetExpressionType() == ExpressionType::COLUMN_REF) {
                auto &cr = static_cast<const ColumnRefExpression &>(e);
                for (size_t i = 0; i < col_names.size(); i++) {
                    if (col_names[i] == cr.column_name) {
                        k.col_idx = static_cast<int>(i);
                        break;
                    }
                }
            }
            keys.push_back(k);
        }

        std::sort(rows.begin(), rows.end(),
            [&](const std::vector<Value> &a, const std::vector<Value> &b) {
                for (auto &k : keys) {
                    if (k.col_idx < 0) continue;
                    if (k.col_idx >= static_cast<int>(a.size()) ||
                        k.col_idx >= static_cast<int>(b.size())) continue;
                    const Value &va = a[k.col_idx];
                    const Value &vb = b[k.col_idx];
                    if (va.IsNull() && vb.IsNull()) continue;
                    if (va.IsNull()) return k.nulls_first;
                    if (vb.IsNull()) return !k.nulls_first;
                    if (va < vb) return k.ascending;
                    if (vb < va) return !k.ascending;
                }
                return false;
            });
    }

    int64_t offset_val = 0;
    int64_t limit_val = -1;
    auto extract_int_literal = [](const ParsedExprPtr &p) -> int64_t {
        if (!p || p->GetExpressionType() != ExpressionType::CONSTANT) return -1;
        auto &c = static_cast<const ConstantExpression &>(*p);
        if (c.literal_type != TokenType::INTEGER_LITERAL) return -1;
        try { return std::stoll(c.value); } catch (...) { return -1; }
    };
    int64_t lit_offset = extract_int_literal(offset_expr);
    int64_t lit_limit = extract_int_literal(limit_expr);
    if (lit_offset > 0) offset_val = lit_offset;
    if (lit_limit >= 0) limit_val = lit_limit;
    if (offset_val > 0 || limit_val >= 0) {
        size_t start = std::min<size_t>(static_cast<size_t>(offset_val), rows.size());
        size_t end = rows.size();
        if (limit_val >= 0) {
            end = std::min<size_t>(start + static_cast<size_t>(limit_val), rows.size());
        }
        std::vector<std::vector<Value>> trimmed;
        if (end > start) trimmed.reserve(end - start);
        for (size_t i = start; i < end; i++) {
            trimmed.push_back(std::move(rows[i]));
        }
        rows = std::move(trimmed);
    }
}

// Resolve the DISPLAY logical type for one SELECT-list expression so a
// Parquet DATE / TIMESTAMP column renders as an ISO string. The engine
// carries DATE/TIMESTAMP columns internally as INTEGER/BIGINT (epoch
// integers) — this re-tag happens only on the final result, downstream of
// every planner and decode path. Returns INVALID when no re-tag applies.
//
//   - column-ref to a Parquet column whose converted_type is DATE /
//     TIMESTAMP_MICROS -> that DATE / TIMESTAMP type.
//   - MIN(col) / MAX(col) of such a column -> the column's date type
//     (MIN/MAX of a date IS a date).
//   - DATE_TRUNC(...) -> TIMESTAMP (its result is a timestamp).
//
// `table` is the single scanned table; JOIN column indices are offsets into
// a combined space, so multi-table selects are skipped (left un-retagged).
static LogicalType ResolveResultDisplayType(const BoundExpression &expr,
                                            const TableCatalogEntry *table) {
    auto parquet_col_display = [&](idx_t col_idx) -> LogicalType {
        if (!table || !table->IsFileScan() ||
            table->GetFileFormat() != "parquet") {
            return LogicalType(LogicalTypeId::INVALID);
        }
        auto reader = table->GetCachedParquetReader();
        if (!reader) return LogicalType(LogicalTypeId::INVALID);
        const auto &disp = reader->GetColumnDisplayTypes();
        if (col_idx >= disp.size()) return LogicalType(LogicalTypeId::INVALID);
        auto id = disp[col_idx].id();
        if (id == LogicalTypeId::DATE || id == LogicalTypeId::TIMESTAMP)
            return disp[col_idx];
        return LogicalType(LogicalTypeId::INVALID);
    };

    if (expr.GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        return parquet_col_display(
            static_cast<const BoundColumnRef &>(expr).column_index);
    }
    if (expr.GetExpressionType() == BoundExpressionType::FUNCTION) {
        auto &fn = static_cast<const BoundFunction &>(expr);
        auto name = StringUtil::Upper(fn.function_name);
        if (name == "DATE_TRUNC") return LogicalType::TIMESTAMP();
        if ((name == "MIN" || name == "MAX") && fn.arguments.size() == 1 &&
            fn.arguments[0]->GetExpressionType() ==
                BoundExpressionType::COLUMN_REF) {
            return parquet_col_display(
                static_cast<const BoundColumnRef &>(*fn.arguments[0])
                    .column_index);
        }
    }
    return LogicalType(LogicalTypeId::INVALID);
}

// If `path_or_url` starts with http://, https://, or s3://, fetch it to a
// system temp file and return the temp path (with the original extension
// preserved so format detection still works). Returns the input unchanged for
// local paths. s3:// is rewritten to virtual-host HTTPS - anonymous read of
// public buckets. No SigV4 / credentials yet.
// Stable, per-URL temp-file name so repeated queries against the same
// remote file reuse the cached download instead of re-fetching the whole
// payload every query. Hashes the URL to keep filenames bounded and
// safe on Windows. The first query on a URL still pays the full
// download cost; every subsequent query uses the cached file.
//
// Cache is process-temp-dir scoped — it persists across queries within
// a process and is reaped naturally by OS temp-dir cleanup. No staleness
// check yet: if the remote file changes, the user must `pip install
// --force-reinstall` or delete the temp file (TODO: HTTP Range request
// the footer + ETag check before reuse).
static std::string ResolveRemoteFile(const std::string &path_or_url) {
    bool is_http  = path_or_url.rfind("http://", 0)  == 0;
    bool is_https = path_or_url.rfind("https://", 0) == 0;
    bool is_s3    = path_or_url.rfind("s3://", 0)    == 0;
    if (!is_http && !is_https && !is_s3) return path_or_url;

    std::string url = is_s3 ? S3Client::S3ToHTTPS(path_or_url) : path_or_url;

    // Preserve the extension so downstream format sniffing picks the right reader.
    std::string ext;
    auto qpos = path_or_url.find('?');
    std::string clean = (qpos == std::string::npos) ? path_or_url : path_or_url.substr(0, qpos);
    auto dot = clean.rfind('.');
    auto slash = clean.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        ext = clean.substr(dot);
    }

    // Stable filename keyed on a 64-bit FNV-1a hash of the URL.
    uint64_t h = 14695981039346656037ULL;
    for (char c : url) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
    char hbuf[32];
    std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                  static_cast<unsigned long long>(h));

    auto tmp = std::filesystem::temp_directory_path() /
               (std::string("slothdb_remote_") + hbuf + ext);
    auto tmp_str = tmp.string();

    // Reuse cached download if present.
    if (std::filesystem::exists(tmp) && std::filesystem::file_size(tmp) > 0) {
        return tmp_str;
    }

    if (!HTTPClient::DownloadToFile(url, tmp_str)) {
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Failed to download: " + url);
    }
    return tmp_str;
}

Connection::Connection(Database &database) : db_(database) {}
Connection::~Connection() = default;

// ============================================================================
// SQL Execution Pipeline: parse -> bind -> plan -> physical plan -> execute
// ============================================================================

QueryResult Connection::Query(const std::string &sql) {
    // 1. Parse.
    auto statements = Parser::Parse(sql);

    QueryResult final_result;
    for (auto &stmt : statements) {
        // Handle transaction control.
        if (stmt->GetType() == StatementType::BEGIN_TXN) {
            BeginTransaction();
            continue;
        }
        if (stmt->GetType() == StatementType::COMMIT_TXN) {
            CommitTransaction();
            continue;
        }
        if (stmt->GetType() == StatementType::ROLLBACK_TXN) {
            RollbackTransaction();
            continue;
        }

        // Handle PRAGMA: read-only catalog introspection for BI tool drivers.
        if (stmt->GetType() == StatementType::PRAGMA) {
            auto &pr = static_cast<PragmaStatement &>(*stmt);
            auto name = StringUtil::Upper(pr.name);
            // Accept both `PRAGMA table_info` and the `PRAGMA pragma_table_info`
            // alias DuckDB exposes for table-valued function use.
            if (name == "PRAGMA_TABLE_INFO") name = "TABLE_INFO";
            if (name == "PRAGMA_DATABASE_LIST") name = "DATABASE_LIST";

            if (name == "TABLE_INFO") {
                if (pr.arg.empty()) {
                    throw BinderException("PRAGMA table_info requires a table name");
                }
                auto *tbl = db_.GetCatalog().GetTable(pr.arg);
                if (!tbl) {
                    throw CatalogException("Table '" + pr.arg + "' not found");
                }
                final_result.column_names = {"cid", "name", "type", "notnull",
                                             "dflt_value", "pk"};
                final_result.column_types = {
                    LogicalType::INTEGER(), LogicalType::VARCHAR(),
                    LogicalType::VARCHAR(), LogicalType::BOOLEAN(),
                    LogicalType::VARCHAR(), LogicalType::BOOLEAN()};
                auto &cols = tbl->GetColumns();
                for (idx_t i = 0; i < cols.size(); i++) {
                    final_result.rows.push_back({
                        Value::INTEGER(static_cast<int32_t>(i)),
                        Value::VARCHAR(cols[i].name),
                        Value::VARCHAR(cols[i].type.ToString()),
                        Value::BOOLEAN(false),
                        Value(),
                        Value::BOOLEAN(false)});
                }
                continue;
            }
            if (name == "DATABASE_LIST") {
                final_result.column_names = {"seq", "name", "file"};
                final_result.column_types = {
                    LogicalType::BIGINT(), LogicalType::VARCHAR(),
                    LogicalType::VARCHAR()};
                // No ATTACH support yet; always the single in-memory database.
                final_result.rows.push_back({
                    Value::BIGINT(0),
                    Value::VARCHAR("memory"),
                    Value()});
                continue;
            }
            throw BinderException("Unknown PRAGMA: " + pr.name);
        }

        // Handle SHOW TABLES / SHOW DATABASES / SHOW COLUMNS FROM t.
        // Standardized across DuckDB / MySQL / ClickHouse; result shape matches
        // DuckDB (1-col for TABLES/DATABASES, 6-col MySQL-compatible for
        // COLUMNS) so bindings get the same UX as the native CLI.
        if (stmt->GetType() == StatementType::SHOW) {
            auto &sh = static_cast<ShowStatement &>(*stmt);

            // Tiny SQL LIKE matcher: '%' = any run, '_' = one char. Enough
            // for pattern filters on SHOW; reuse across all three kinds.
            auto like_match = [](const std::string &name, const std::string &pat) {
                if (pat.empty()) return true;
                size_t ni = 0, pi = 0, star_n = std::string::npos, star_p = 0;
                while (ni < name.size()) {
                    if (pi < pat.size() && pat[pi] == '%') {
                        star_p = ++pi; star_n = ni;
                    } else if (pi < pat.size() &&
                               (pat[pi] == '_' || pat[pi] == name[ni])) {
                        pi++; ni++;
                    } else if (star_n != std::string::npos) {
                        pi = star_p; ni = ++star_n;
                    } else {
                        return false;
                    }
                }
                while (pi < pat.size() && pat[pi] == '%') pi++;
                return pi == pat.size();
            };

            if (sh.kind == ShowStatement::Kind::TABLES) {
                final_result.column_names = {"name"};
                final_result.column_types = {LogicalType::VARCHAR()};
                auto &schema = db_.GetCatalog().GetSchema();
                for (const auto &name : schema.GetTableNames()) {
                    if (like_match(name, sh.like_pattern)) {
                        final_result.rows.push_back({Value::VARCHAR(name)});
                    }
                }
                continue;
            }

            if (sh.kind == ShowStatement::Kind::DATABASES) {
                final_result.column_names = {"database_name"};
                final_result.column_types = {LogicalType::VARCHAR()};
                // No ATTACH yet; always the single in-memory database.
                if (like_match("memory", sh.like_pattern)) {
                    final_result.rows.push_back({Value::VARCHAR("memory")});
                }
                continue;
            }

            // SHOW COLUMNS FROM t: same 6-col shape as DESCRIBE.
            auto *tbl = db_.GetCatalog().GetTable(sh.table_name);
            if (!tbl) throw CatalogException("Table '" + sh.table_name + "' not found");
            final_result.column_names = {"column_name", "column_type", "null",
                                         "key", "default", "extra"};
            final_result.column_types = {
                LogicalType::VARCHAR(), LogicalType::VARCHAR(),
                LogicalType::VARCHAR(), LogicalType::VARCHAR(),
                LogicalType::VARCHAR(), LogicalType::VARCHAR()};
            for (const auto &c : tbl->GetColumns()) {
                if (!sh.like_pattern.empty() && !like_match(c.name, sh.like_pattern)) continue;
                final_result.rows.push_back({
                    Value::VARCHAR(c.name),
                    Value::VARCHAR(c.type.ToString()),
                    Value::VARCHAR("YES"),
                    Value(), Value(), Value()});
            }
            continue;
        }

        // Handle DESCRIBE: bind the inner statement and emit its result schema.
        if (stmt->GetType() == StatementType::DESCRIBE) {
            auto &desc = static_cast<DescribeStatement &>(*stmt);
            std::vector<std::string> names;
            std::vector<LogicalType> types;

            // Fast path for DESCRIBE '<file>' (file literal). The binder
            // doesn't know about the parser's synthetic __FILE__ table
            // function, but the SELECT pipeline at the bottom of this loop
            // does all the format auto-detection already. Just peek at
            // column names/types via `SELECT * FROM '<path>' LIMIT 0` and
            // reuse the existing rewrite path. ClickHouse parity: this is
            // the "describe a file without importing" feature.
            bool handled_via_peek = false;
            if (desc.inner && desc.inner->GetType() == StatementType::SELECT) {
                auto &sel = static_cast<SelectStatement &>(*desc.inner);
                if (sel.from_table && sel.from_table->is_table_function &&
                    sel.from_table->table_name == "__FILE__" &&
                    !sel.from_table->function_args.empty() &&
                    sel.from_table->function_args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto path = static_cast<ConstantExpression &>(
                        *sel.from_table->function_args[0]).value;
                    auto peek = Query("SELECT * FROM '" + path + "' LIMIT 0");
                    names = peek.column_names;
                    types = peek.column_types;
                    handled_via_peek = true;
                }
            }

            if (!handled_via_peek) {
                Binder binder(db_.GetCatalog());
                auto bound = binder.Bind(*desc.inner);
                if (bound->GetType() != BoundStatementType::SELECT) {
                    throw BinderException("DESCRIBE is only supported for SELECT queries");
                }
                auto &sel = static_cast<BoundSelectStatement &>(*bound);
                names = sel.result_names;
                types = sel.result_types;
            }

            final_result.column_names = {"column_name", "column_type", "null",
                                         "key", "default", "extra"};
            final_result.column_types = {
                LogicalType::VARCHAR(), LogicalType::VARCHAR(),
                LogicalType::VARCHAR(), LogicalType::VARCHAR(),
                LogicalType::VARCHAR(), LogicalType::VARCHAR()};
            for (idx_t i = 0; i < names.size(); i++) {
                final_result.rows.push_back({
                    Value::VARCHAR(names[i]),
                    Value::VARCHAR(types[i].ToString()),
                    Value::VARCHAR("YES"),
                    Value(), Value(), Value()});
            }
            continue;
        }

        // Handle EXPLAIN: bind and plan the inner statement, return plan as text.
        if (stmt->GetType() == StatementType::EXPLAIN) {
            auto &explain = static_cast<ExplainStatement &>(*stmt);
            Binder binder(db_.GetCatalog());
            auto bound = binder.Bind(*explain.inner);
            auto logical = Planner::Plan(*bound);

            // Build a simple plan description.
            std::string plan_text;
            std::function<void(const LogicalOperator &, int)> describe;
            describe = [&](const LogicalOperator &op, int indent) {
                for (int i = 0; i < indent; i++) plan_text += "  ";
                auto type = op.GetOperatorType();
                switch (type) {
                case LogicalOperatorType::GET: plan_text += "SCAN"; break;
                case LogicalOperatorType::FILTER: plan_text += "FILTER"; break;
                case LogicalOperatorType::PROJECTION: plan_text += "PROJECTION"; break;
                case LogicalOperatorType::ORDER_BY: plan_text += "ORDER_BY"; break;
                case LogicalOperatorType::LIMIT: plan_text += "LIMIT"; break;
                case LogicalOperatorType::AGGREGATE: plan_text += "AGGREGATE"; break;
                case LogicalOperatorType::JOIN: plan_text += "JOIN"; break;
                case LogicalOperatorType::DISTINCT: plan_text += "DISTINCT"; break;
                case LogicalOperatorType::WINDOW: plan_text += "WINDOW"; break;
                case LogicalOperatorType::INSERT: plan_text += "INSERT"; break;
                case LogicalOperatorType::CREATE_TABLE: plan_text += "CREATE_TABLE"; break;
                case LogicalOperatorType::DROP_TABLE: plan_text += "DROP_TABLE"; break;
                case LogicalOperatorType::UPDATE: plan_text += "UPDATE"; break;
                case LogicalOperatorType::DELETE_STMT: plan_text += "DELETE"; break;
                case LogicalOperatorType::DUMMY_SCAN: plan_text += "DUMMY_SCAN"; break;
                }
                plan_text += "\n";
                for (auto &child : op.children) {
                    describe(*child, indent + 1);
                }
            };
            describe(*logical, 0);

            final_result.column_names = {"plan"};
            final_result.column_types = {LogicalType::VARCHAR()};
            final_result.rows.push_back({Value::VARCHAR(plan_text)});
            continue;
        }

        // Handle CREATE TABLE AS SELECT (CTAS): execute the SELECT, materialize
        // the result into a new table. Schema is inferred from the query's
        // result types/names. Mirrors the CREATE VIEW materialization path but
        // without SetViewQuery - this is a real table, not a virtual view.
        if (stmt->GetType() == StatementType::CREATE_TABLE) {
            auto &ct = static_cast<CreateTableStatement &>(*stmt);
            if (ct.query) {
                if (ct.if_not_exists && db_.GetCatalog().GetTable(ct.table_name)) {
                    continue;
                }

                // Bind/Plan/Execute the parsed SELECT directly. The
                // previous implementation extracted the SELECT text
                // via a `" AS "` substring search on the *raw input
                // string*, which silently captured any trailing
                // statements (e.g. `; SELECT * FROM dst`) into the
                // CTAS body. Multi-statement CTAS then threw
                // "Table 'dst' not found" because the trailing select
                // ran against a catalog where the new table didn't
                // exist yet. Mirrors the INSERT...SELECT pipeline.
                Binder sel_binder(db_.GetCatalog());
                auto sel_bound = sel_binder.Bind(*ct.query);
                auto sel_logical = Planner::Plan(*sel_bound);
                PhysicalPlanner sel_pp(db_.GetCatalog());
                auto sel_physical = sel_pp.Plan(*sel_logical);
                sel_physical->Init();

                auto &sel_select = static_cast<BoundSelectStatement &>(*sel_bound);
                std::vector<std::string> col_names = sel_select.result_names;
                std::vector<LogicalType> col_types = sel_select.result_types;

                if (ct.or_replace) db_.GetCatalog().DropTable(ct.table_name);
                if (db_.GetCatalog().GetTable(ct.table_name)) {
                    throw CatalogException("Table '" + ct.table_name +
                                           "' already exists");
                }

                std::vector<ColumnDefinition> cols;
                cols.reserve(col_names.size());
                for (idx_t i = 0; i < col_names.size(); i++) {
                    cols.emplace_back(col_names[i], col_types[i]);
                }
                auto &entry = db_.GetCatalog().CreateTable(ct.table_name,
                                                           std::move(cols));
                auto storage = std::make_shared<DataTable>(col_types);
                DataChunk chunk;
                while (sel_physical->GetData(chunk)) {
                    storage->Append(chunk);
                }
                entry.SetStorage(storage);
                continue;
            }
        }

        // Handle CREATE VIEW: store the SQL query for virtual re-execution.
        if (stmt->GetType() == StatementType::CREATE_VIEW) {
            auto &cv = static_cast<CreateViewStatement &>(*stmt);

            // Extract the SELECT SQL from the original statement.
            // Find "AS" in the SQL and take everything after it.
            std::string view_sql;
            {
                auto upper_sql = StringUtil::Upper(sql);
                auto as_pos = upper_sql.find(" AS ");
                if (as_pos != std::string::npos) {
                    view_sql = sql.substr(as_pos + 4);
                    // Trim trailing semicolons and whitespace.
                    while (!view_sql.empty() && (view_sql.back() == ';' || view_sql.back() == ' ' || view_sql.back() == '\n' || view_sql.back() == '\r'))
                        view_sql.pop_back();
                }
            }

            // For LIVE views, extract the single file path from the parsed
            // SELECT's FROM clause. Supports `FROM 'path'` (parser emits
            // __FILE__ + ConstantExpression) and `read_csv/parquet/json/
            // avro/arrow/xlsx('path')`. Complex FROM shapes (JOINs, set
            // operations, multi-file globs) are rejected for the MVP.
            std::string live_file_path;
            bool live_incremental = false;
            char live_incremental_delim = ',';
            if (cv.is_live) {
                auto extract_path = [](TableRef *ref) -> std::string {
                    if (!ref || !ref->is_table_function) return {};
                    if (ref->function_args.empty()) return {};
                    if (ref->function_args[0]->GetExpressionType() != ExpressionType::CONSTANT) return {};
                    auto up = StringUtil::Upper(ref->table_name);
                    if (up != "__FILE__" && up != "READ_CSV" && up != "READ_PARQUET" &&
                        up != "READ_JSON" && up != "READ_AVRO" && up != "READ_ARROW" &&
                        up != "READ_XLSX") return {};
                    return static_cast<ConstantExpression &>(*ref->function_args[0]).value;
                };
                if (cv.query && cv.query->from_table) {
                    live_file_path = extract_path(cv.query->from_table.get());
                }
                if (live_file_path.empty()) {
                    throw BinderException("CREATE LIVE VIEW requires a single "
                        "file source (e.g. FROM 'app.log' or read_csv('data.csv'))");
                }
                // v2 incremental-append eligibility. The view must be a
                // trivial pass-through (`SELECT * FROM 'file'`) over a
                // CSV-shaped source - WHERE / GROUP BY / ORDER BY / JOIN /
                // projection would make "parse the tail and append" wrong.
                // Other shapes fall back to the v1 full-rescan path.
                if (cv.query) {
                    auto &q = *cv.query;
                    bool shape_ok =
                        q.select_list.size() == 1 &&
                        q.select_list[0] &&
                        q.select_list[0]->GetExpressionType() == ExpressionType::STAR &&
                        !q.where_clause && q.group_by.empty() &&
                        q.order_by.empty() && !q.set_right &&
                        !q.having_clause && !q.is_distinct &&
                        !q.limit;
                    auto dot = live_file_path.find_last_of('.');
                    std::string ext = (dot == std::string::npos)
                        ? "" : live_file_path.substr(dot + 1);
                    for (auto &c : ext) c = static_cast<char>(std::tolower(c));
                    if (shape_ok && (ext == "csv" || ext == "tsv")) {
                        live_incremental = true;
                        live_incremental_delim = (ext == "tsv") ? '\t' : ',';
                    }
                }
            }

            // Execute the view query once to determine column names and types.
            auto view_result = Query(view_sql);

            if (cv.or_replace) db_.GetCatalog().DropTable(cv.view_name);

            std::vector<ColumnDefinition> cols;
            for (idx_t i = 0; i < view_result.column_names.size(); i++) {
                cols.emplace_back(view_result.column_names[i], view_result.column_types[i]);
            }

            auto &entry = db_.GetCatalog().CreateTable(cv.view_name, std::move(cols));
            auto storage = std::make_shared<DataTable>(view_result.column_types);
            view_result.MaterialiseRows();
            BulkLoadRows(*storage, view_result.column_types, view_result.rows);
            entry.SetStorage(storage);
            entry.SetViewQuery(view_sql);
            if (cv.is_live) {
                entry.MarkLiveView();
                entry.SetWatchedFile(live_file_path);
                std::error_code ec;
                auto t = std::filesystem::last_write_time(live_file_path, ec);
                if (!ec) {
                    entry.SetCachedMTime(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t.time_since_epoch()).count());
                }
                // Capture size + header sentinel for the incremental path.
                // The sentinel (first 64 bytes) protects against mistaking a
                // rewrite-in-place for an append when sizes happen to line up.
                std::error_code ec2;
                auto sz = std::filesystem::file_size(live_file_path, ec2);
                if (!ec2) {
                    entry.SetLastSize(static_cast<int64_t>(sz));
                    std::ifstream in(live_file_path, std::ios::binary);
                    char head[64] = {0};
                    in.read(head, sizeof(head));
                    entry.SetFirstBytes(std::string(head, in.gcount()));
                }
                if (live_incremental) {
                    entry.MarkIncrementalEligible(live_incremental_delim);
                }
                entry.MarkCacheValid();
            }
            continue;
        }

        // Handle TRUNCATE: replace storage with empty table.
        if (stmt->GetType() == StatementType::TRUNCATE) {
            auto &trunc = static_cast<TruncateStatement &>(*stmt);
            auto *entry = db_.GetCatalog().GetTable(trunc.table_name);
            if (!entry) throw CatalogException("Table '" + trunc.table_name + "' not found");
            entry->SetStorage(std::make_shared<DataTable>(entry->GetTypes()));
            continue;
        }

        // Handle ALTER TABLE.
        if (stmt->GetType() == StatementType::ALTER_TABLE) {
            auto &alter = static_cast<AlterTableStatement &>(*stmt);
            auto *entry = db_.GetCatalog().GetTable(alter.table_name);
            if (!entry) throw CatalogException("Table '" + alter.table_name + "' not found");

            auto old_cols = entry->GetColumns();
            auto old_types = entry->GetTypes();

            if (alter.action == AlterTableStatement::AlterAction::ADD_COLUMN) {
                // Scan all data, add NULL for new column, rebuild.
                Binder binder(db_.GetCatalog());
                auto new_type = binder.ResolveTypeName(alter.column_type);

                std::vector<ColumnDefinition> new_cols = old_cols;
                new_cols.emplace_back(alter.column_name, new_type);
                auto new_types = old_types;
                new_types.push_back(new_type);

                auto new_storage = std::make_shared<DataTable>(new_types);
                auto &old_storage = entry->GetStorage();
                auto state = old_storage.InitScan();
                DataChunk chunk;
                while (true) {
                    chunk.Initialize(old_types);
                    if (!old_storage.Scan(state, chunk)) break;
                    DataChunk new_chunk;
                    new_chunk.Initialize(new_types);
                    for (idx_t i = 0; i < chunk.size(); i++) {
                        for (idx_t c = 0; c < old_types.size(); c++)
                            new_chunk.SetValue(c, i, chunk.GetValue(c, i));
                        new_chunk.SetValue(old_types.size(), i, Value()); // NULL
                    }
                    new_storage->Append(new_chunk);
                }

                // Replace the table entry.
                db_.GetCatalog().DropTable(alter.table_name);
                auto &new_entry = db_.GetCatalog().CreateTable(alter.table_name, std::move(new_cols));
                new_entry.SetStorage(new_storage);
            } else if (alter.action == AlterTableStatement::AlterAction::DROP_COLUMN) {
                auto drop_idx = entry->GetColumnIndex(alter.column_name);
                if (drop_idx == INVALID_INDEX)
                    throw CatalogException("Column '" + alter.column_name + "' not found");

                std::vector<ColumnDefinition> new_cols;
                std::vector<LogicalType> new_types;
                for (idx_t c = 0; c < old_cols.size(); c++) {
                    if (c != drop_idx) {
                        new_cols.push_back(old_cols[c]);
                        new_types.push_back(old_types[c]);
                    }
                }

                auto new_storage = std::make_shared<DataTable>(new_types);
                auto &old_storage = entry->GetStorage();
                auto state = old_storage.InitScan();
                DataChunk chunk;
                while (true) {
                    chunk.Initialize(old_types);
                    if (!old_storage.Scan(state, chunk)) break;
                    DataChunk new_chunk;
                    new_chunk.Initialize(new_types);
                    for (idx_t i = 0; i < chunk.size(); i++) {
                        idx_t nc = 0;
                        for (idx_t c = 0; c < old_types.size(); c++) {
                            if (c != drop_idx)
                                new_chunk.SetValue(nc++, i, chunk.GetValue(c, i));
                        }
                    }
                    new_storage->Append(new_chunk);
                }

                db_.GetCatalog().DropTable(alter.table_name);
                auto &new_entry = db_.GetCatalog().CreateTable(alter.table_name, std::move(new_cols));
                new_entry.SetStorage(new_storage);
            } else if (alter.action == AlterTableStatement::AlterAction::RENAME_COLUMN) {
                auto rename_idx = entry->GetColumnIndex(alter.column_name);
                if (rename_idx == INVALID_INDEX)
                    throw CatalogException("Column '" + alter.column_name + "' not found");

                std::vector<ColumnDefinition> new_cols = old_cols;
                new_cols[rename_idx].name = alter.new_column_name;


                // We need the actual shared_ptr. Just rebuild.
                auto new_storage = std::make_shared<DataTable>(old_types);
                auto &old_storage = entry->GetStorage();
                auto state = old_storage.InitScan();
                DataChunk chunk;
                while (true) {
                    chunk.Initialize(old_types);
                    if (!old_storage.Scan(state, chunk)) break;
                    new_storage->Append(chunk);
                }

                db_.GetCatalog().DropTable(alter.table_name);
                auto &new_entry = db_.GetCatalog().CreateTable(alter.table_name, std::move(new_cols));
                new_entry.SetStorage(new_storage);
            }
            continue;
        }

        // Handle INSERT INTO ... SELECT.
        if (stmt->GetType() == StatementType::INSERT) {
            auto &ins = static_cast<InsertStatement &>(*stmt);
            if (ins.select_source) {
                // Execute the SELECT.
                Binder sel_binder(db_.GetCatalog());
                auto sel_bound = sel_binder.Bind(*ins.select_source);
                auto sel_logical = Planner::Plan(*sel_bound);
                PhysicalPlanner sel_pp(db_.GetCatalog());
                auto sel_physical = sel_pp.Plan(*sel_logical);
                sel_physical->Init();

                auto *entry = db_.GetCatalog().GetTable(ins.table_name);
                if (!entry) throw CatalogException("Table '" + ins.table_name + "' not found");

                auto col_types = entry->GetTypes();
                // Resolve user column-list to table-column indices.
                // Empty list means positional (declaration order).
                // Mirrors the INSERT VALUES path fixed in 292527f —
                // without this, reordered or subset INSERT...SELECT
                // silently corrupted rows (or crashed on subset).
                std::vector<idx_t> target_indices;
                if (!ins.column_names.empty()) {
                    target_indices.reserve(ins.column_names.size());
                    std::vector<bool> seen(col_types.size(), false);
                    for (auto &col_name : ins.column_names) {
                        idx_t idx = entry->GetColumnIndex(col_name);
                        if (idx == INVALID_INDEX) {
                            throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                                "Column '" + col_name + "' not found in table '" +
                                ins.table_name + "'");
                        }
                        if (seen[idx]) {
                            throw BinderException(ErrorCode::TYPE_MISMATCH,
                                "Column '" + col_name + "' specified more than once "
                                "in INSERT column list");
                        }
                        seen[idx] = true;
                        target_indices.push_back(idx);
                    }
                }

                // Validate SELECT arity against the expected target.
                auto &sel_select = static_cast<BoundSelectStatement &>(*sel_bound);
                idx_t expected_cols = target_indices.empty()
                    ? col_types.size() : target_indices.size();
                if (sel_select.result_types.size() != expected_cols) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "INSERT SELECT column count (" +
                        std::to_string(sel_select.result_types.size()) +
                        ") does not match " +
                        (target_indices.empty() ? "table"
                                                : "column list") +
                        " count (" + std::to_string(expected_cols) + ")");
                }

                DataChunk chunk;
                while (true) {
                    if (!sel_physical->GetData(chunk)) break;
                    if (target_indices.empty()) {
                        entry->GetStorage().Append(chunk);
                    } else {
                        // Permute: SELECT column i lands at table
                        // column target_indices[i]. Unspecified columns
                        // get an all-NULL vector of the column's type.
                        DataChunk permuted;
                        permuted.Initialize(col_types);
                        permuted.SetCardinality(chunk.size());
                        for (idx_t c = 0; c < col_types.size(); c++) {
                            // Default: fill with NULL.
                            for (idx_t r = 0; r < chunk.size(); r++) {
                                permuted.GetVector(c).GetValidity().SetInvalid(r);
                            }
                        }
                        for (idx_t i = 0; i < target_indices.size(); i++) {
                            idx_t tgt = target_indices[i];
                            for (idx_t r = 0; r < chunk.size(); r++) {
                                permuted.SetValue(tgt, r, chunk.GetValue(i, r));
                            }
                        }
                        entry->GetStorage().Append(permuted);
                    }
                }
                continue;
            }
        }

        // Handle COPY (CSV, JSON formats).
        if (stmt->GetType() == StatementType::COPY) {
            auto &copy = static_cast<CopyStatement &>(*stmt);

            // If COPY (SELECT ...) TO 'file', materialize subquery into a temp table first.
            if (copy.source_query) {
                // Inline read_csv expansion for the subquery's from clause.
                auto expand_csv = [&](TableRef &tref) {
                    if (!tref.is_table_function) return;
                    auto upper = StringUtil::Upper(tref.table_name);
                    if (upper != "READ_CSV" && upper != "READ_CSV_AUTO") return;
                    auto &args = tref.function_args;
                    if (args.empty() || args[0]->GetExpressionType() != ExpressionType::CONSTANT) return;
                    auto file_pattern = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);
                    auto files = FileGlob::Glob(file_pattern);
                    if (files.empty()) throw IOException("No files: " + file_pattern);
                    FastCSVReader r(files[0], ',', true, 65536);
                    auto header = r.ReadHeader();
                    auto types_csv = r.DetectTypes();
                    static int copy_uid = 0;
                    std::string tn = "__copy_csv_" + std::to_string(++copy_uid) + "__";
                    tref.table_name = tn;
                    tref.is_table_function = false;
                    std::vector<ColumnDefinition> cs;
                    for (size_t i = 0; i < header.size() && i < types_csv.size(); i++)
                        cs.emplace_back(header[i], types_csv[i]);
                    auto &e = db_.GetCatalog().CreateTable(tn, std::move(cs));
                    auto st = std::make_shared<DataTable>(types_csv);
                    e.SetStorage(st);
                    e.SetFilePath(files[0]);
                };
                // Rewrite bare file-literal FROM clauses (__FILE__ from the
                // parser) into read_xxx() form so the downstream expand_csv
                // can pick them up. Currently supports CSV / TSV sources -
                // Parquet / JSON COPY-from will come with the full
                // preprocessing refactor.
                auto rewrite_file_ref = [&](TableRef &tref) {
                    if (!tref.is_table_function) return;
                    if (tref.table_name != "__FILE__") return;
                    if (tref.function_args.empty()) return;
                    if (tref.function_args[0]->GetExpressionType() != ExpressionType::CONSTANT) return;
                    auto path = static_cast<ConstantExpression &>(*tref.function_args[0]).value;
                    auto dot = path.find_last_of('.');
                    if (dot == std::string::npos) return;
                    std::string ext = path.substr(dot + 1);
                    for (auto &c : ext) c = static_cast<char>(std::tolower(c));
                    if (ext == "csv" || ext == "tsv") {
                        tref.table_name = "READ_CSV";
                    }
                    // Leave Parquet / JSON / etc. as __FILE__ so the binder
                    // surfaces a clear error instead of a silent mis-read.
                };
                if (copy.source_query->from_table) {
                    rewrite_file_ref(*copy.source_query->from_table);
                    if (copy.source_query->from_table->right) rewrite_file_ref(*copy.source_query->from_table->right);
                    expand_csv(*copy.source_query->from_table);
                    if (copy.source_query->from_table->right) expand_csv(*copy.source_query->from_table->right);
                }
                Binder binder(db_.GetCatalog());
                auto bound = binder.Bind(*copy.source_query);
                auto logical = Planner::Plan(*bound);
                logical = Optimizer::Optimize(std::move(logical));
                PhysicalPlanner pp(db_.GetCatalog());
                auto physical = pp.Plan(*logical);
                physical->Init();
                auto &sel = static_cast<BoundSelectStatement &>(*bound);
                std::string tmp_name = "__copy_src_" + std::to_string(reinterpret_cast<uintptr_t>(physical.get())) + "__";
                std::vector<ColumnDefinition> cols;
                for (idx_t i = 0; i < sel.result_names.size(); i++)
                    cols.emplace_back(sel.result_names[i], sel.result_types[i]);
                if (db_.GetCatalog().GetTable(tmp_name)) db_.GetCatalog().DropTable(tmp_name);
                auto &entry_tmp = db_.GetCatalog().CreateTable(tmp_name, std::move(cols));
                auto storage = std::make_shared<DataTable>(sel.result_types);
                entry_tmp.SetStorage(storage);
                DataChunk chunk;
                while (physical->GetData(chunk)) {
                    storage->Append(chunk);
                }
                copy.table_name = tmp_name;
            }

            auto *entry = db_.GetCatalog().GetTable(copy.table_name);
            if (!entry) throw CatalogException("Table '" + copy.table_name + "' not found");
            auto types = entry->GetTypes();

            // Auto-detect format from file extension if not specified.
            auto fmt = StringUtil::Upper(copy.format);
            if (fmt == "CSV") {
                auto ext = copy.file_path.substr(copy.file_path.find_last_of('.') + 1);
                for (auto &c : ext) c = static_cast<char>(std::tolower(c));
                if (ext == "json" || ext == "ndjson") fmt = "JSON";
                if (ext == "parquet") fmt = "PARQUET";
            }

            if (copy.is_from) {
                if (fmt == "PARQUET") {
                    ParquetReader reader(copy.file_path);
                    auto rows = reader.ReadAll();
                    BulkLoadRows(entry->GetStorage(), types, rows);
                } else if (fmt == "JSON") {
                    JSONReader reader(copy.file_path);
                    reader.DetectSchema();
                    auto rows = reader.ReadAll();
                    BulkLoadRows(entry->GetStorage(), types, rows);
                } else {
                    // CSV (default) - fast stream.
                    FastCSVReader reader(copy.file_path, copy.delimiter, copy.header);
                    if (copy.header) reader.ReadHeader();
                    reader.ReadIntoTable(entry->GetStorage(), types);
                }
            } else {
                // COPY TO: export.
                std::vector<std::string> col_names;
                for (auto &col : entry->GetColumns()) col_names.push_back(col.name);

                if (fmt == "PARQUET") {
                    ParquetWriter writer(copy.file_path, col_names, types);
                    auto state = entry->GetStorage().InitScan();
                    DataChunk chunk;
                    std::vector<std::vector<Value>> batch;
                    while (true) {
                        chunk.Initialize(types);
                        if (!entry->GetStorage().Scan(state, chunk)) break;
                        for (idx_t i = 0; i < chunk.size(); i++) {
                            std::vector<Value> row;
                            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                row.push_back(chunk.GetValue(c, i));
                            batch.push_back(std::move(row));
                        }
                    }
                    writer.WriteRowGroup(batch);
                    writer.Finish();
                } else if (fmt == "JSON") {
                    JSONWriter writer(copy.file_path);
                    writer.WriteHeader(col_names);
                    auto state = entry->GetStorage().InitScan();
                    DataChunk chunk;
                    while (true) {
                        chunk.Initialize(types);
                        if (!entry->GetStorage().Scan(state, chunk)) break;
                        for (idx_t i = 0; i < chunk.size(); i++) {
                            std::vector<Value> row;
                            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                row.push_back(chunk.GetValue(c, i));
                            writer.WriteRow(col_names, row);
                        }
                    }
                    writer.Finish();
                } else {
                    // CSV (default).
                    CSVOptions opts;
                    opts.delimiter = copy.delimiter;
                    opts.header = copy.header;
                    CSVWriter writer(copy.file_path, opts);
                    if (opts.header) writer.WriteHeader(col_names);
                    auto state = entry->GetStorage().InitScan();
                    DataChunk chunk;
                    while (true) {
                        chunk.Initialize(types);
                        if (!entry->GetStorage().Scan(state, chunk)) break;
                        for (idx_t i = 0; i < chunk.size(); i++) {
                            std::vector<Value> row;
                            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                row.push_back(chunk.GetValue(c, i));
                            writer.WriteRow(row);
                        }
                    }
                    writer.Flush();
                }
            }
            continue;
        }

        // Handle MERGE.
        if (stmt->GetType() == StatementType::MERGE) {
            auto &merge = static_cast<MergeStatement &>(*stmt);
            auto *target = db_.GetCatalog().GetTable(merge.target_table);
            auto *source = db_.GetCatalog().GetTable(merge.source_table);
            if (!target) throw CatalogException("Target table '" + merge.target_table + "' not found");
            if (!source) throw CatalogException("Source table '" + merge.source_table + "' not found");

            auto target_types = target->GetTypes();
            auto source_types = source->GetTypes();

            // Collect all source rows.
            std::vector<std::vector<Value>> source_rows;
            auto src_state = source->GetStorage().InitScan();
            DataChunk src_chunk;
            while (true) {
                src_chunk.Initialize(source_types);
                if (!source->GetStorage().Scan(src_state, src_chunk)) break;
                for (idx_t i = 0; i < src_chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < src_chunk.ColumnCount(); c++)
                        row.push_back(src_chunk.GetValue(c, i));
                    source_rows.push_back(std::move(row));
                }
            }

            // Collect all target rows.
            std::vector<std::vector<Value>> target_rows;
            auto tgt_state = target->GetStorage().InitScan();
            DataChunk tgt_chunk;
            while (true) {
                tgt_chunk.Initialize(target_types);
                if (!target->GetStorage().Scan(tgt_state, tgt_chunk)) break;
                for (idx_t i = 0; i < tgt_chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < tgt_chunk.ColumnCount(); c++)
                        row.push_back(tgt_chunk.GetValue(c, i));
                    target_rows.push_back(std::move(row));
                }
            }

            // For each source row, check if it matches any target row.
            // Simple approach: extract join columns from ON condition.
            idx_t target_join_col = 0, source_join_col = 0;
            if (merge.on_condition &&
                merge.on_condition->GetExpressionType() == ExpressionType::COMPARISON) {
                auto &cmp = static_cast<ComparisonExpression &>(*merge.on_condition);
                if (cmp.left->GetExpressionType() == ExpressionType::COLUMN_REF) {
                    auto &col = static_cast<ColumnRefExpression &>(*cmp.left);
                    target_join_col = target->GetColumnIndex(col.column_name);
                }
                if (cmp.right->GetExpressionType() == ExpressionType::COLUMN_REF) {
                    auto &col = static_cast<ColumnRefExpression &>(*cmp.right);
                    source_join_col = source->GetColumnIndex(col.column_name);
                    if (source_join_col == INVALID_INDEX) source_join_col = 0;
                }
            }

            auto new_storage = std::make_shared<DataTable>(target_types);
            std::vector<bool> target_matched(target_rows.size(), false);

            for (auto &src_row : source_rows) {
                bool matched = false;
                for (idx_t t = 0; t < target_rows.size(); t++) {
                    if (target_rows[t][target_join_col].ToString() ==
                        src_row[source_join_col].ToString()) {
                        matched = true;
                        target_matched[t] = true;
                        // WHEN MATCHED: update the target row.
                        if (merge.has_update) {
                            for (auto &assign : merge.update_assignments) {
                                auto idx = target->GetColumnIndex(assign.column_name);
                                if (idx != INVALID_INDEX && assign.value &&
                                    assign.value->GetExpressionType() == ExpressionType::COLUMN_REF) {
                                    auto &ref = static_cast<ColumnRefExpression &>(*assign.value);
                                    auto src_idx = source->GetColumnIndex(ref.column_name);
                                    if (src_idx != INVALID_INDEX)
                                        target_rows[t][idx] = src_row[src_idx];
                                }
                            }
                        }
                    }
                }
                // WHEN NOT MATCHED: insert.
                if (!matched && merge.has_insert) {
                    std::vector<Value> new_row(target_types.size());
                    for (idx_t c = 0; c < merge.insert_values.size() && c < target_types.size(); c++) {
                        if (merge.insert_values[c]->GetExpressionType() == ExpressionType::COLUMN_REF) {
                            auto &ref = static_cast<ColumnRefExpression &>(*merge.insert_values[c]);
                            auto src_idx = source->GetColumnIndex(ref.column_name);
                            if (src_idx != INVALID_INDEX) new_row[c] = src_row[src_idx];
                        } else if (merge.insert_values[c]->GetExpressionType() == ExpressionType::CONSTANT) {
                            auto &con = static_cast<ConstantExpression &>(*merge.insert_values[c]);
                            if (!con.is_null) new_row[c] = Value::VARCHAR(con.value);
                        }
                    }
                    target_rows.push_back(std::move(new_row));
                }
            }

            // Rebuild target table.
            for (auto &row : target_rows) {
                DataChunk chunk;
                chunk.Initialize(target_types);
                for (idx_t c = 0; c < row.size(); c++)
                    chunk.SetValue(c, 0, row[c]);
                new_storage->Append(chunk);
            }
            target->SetStorage(new_storage);
            continue;
        }

        // Handle virtual views: if FROM references a view, re-execute the stored query
        // and replace the view's storage with fresh data from the file.
        std::vector<std::string> temp_tables;
        std::vector<std::string> cte_tables;
        std::vector<std::string> _cte_temp_tables;

        // RAII cleanup - drops any temp / CTE tables created during
        // preprocessing even if binding / planning / execution throws.
        // Without this, a failing query leaves a zombie __auto_file__
        // in the catalog that collides on the next query.
        struct TableCleanupGuard {
            Catalog *catalog;
            std::vector<std::string> *tt;
            std::vector<std::string> *ct;
            std::vector<std::string> *ctt;
            ~TableCleanupGuard() {
                auto drop = [&](std::vector<std::string> &v) {
                    for (auto &t : v) {
                        if (catalog->GetTable(t)) catalog->DropTable(t);
                    }
                };
                drop(*ctt);
                drop(*tt);
                drop(*ct);
            }
        } _cleanup{&db_.GetCatalog(), &temp_tables, &cte_tables, &_cte_temp_tables};

        if (stmt->GetType() == StatementType::SELECT) {
            auto &outer_sel = static_cast<SelectStatement &>(*stmt);
            // File-table-function preprocessing (view expansion, read_csv,
            // read_parquet, read_json, read_arrow, read_avro, read_xlsx,
            // sqlite_scan, __FILE__ auto-detect) has to run for every SELECT
            // in a UNION / INTERSECT / EXCEPT chain. Without this, a file
            // literal on the right-hand side reaches the binder as __FILE__
            // and fails with "Table '__FILE__' not found".
            // Counter for auto-generated temp-table names - ensures left and
            // right sides of a UNION (or repeated FROM '...' usages) don't
            // collide on the same __auto_file__ / __read_parquet__ entry.
            static thread_local int preproc_uid = 0;

            // ----------------------------------------------------------
            // Pre-pass: recursively materialize __FILE__ refs anywhere
            // in the query tree (subqueries, CTEs, EXPLAIN inner) into
            // temp catalog tables. The existing top-level processing
            // below only walks `sel.from_table` + JOIN chain at the
            // outermost SELECT; without this pre-pass, nested file
            // refs hit the binder as the literal table name "__FILE__"
            // and fail.
            //
            // Scope: handles CSV / Parquet / JSON. Other formats
            // (Avro / Arrow / Xlsx / SQLite) at nested level still
            // need work — TODO for follow-up.
            // ----------------------------------------------------------
            auto materialize_nested_file = [&](TableRef &tref) {
                if (!tref.is_table_function) return;
                if (tref.table_name != "__FILE__") return;
                if (tref.function_args.empty()) return;
                if (tref.function_args[0]->GetExpressionType() != ExpressionType::CONSTANT) return;

                auto file_path = ResolveRemoteFile(
                    static_cast<ConstantExpression &>(*tref.function_args[0]).value);
                auto ext = file_path.substr(file_path.find_last_of('.') + 1);
                for (auto &c : ext) c = static_cast<char>(std::tolower(c));

                // Always use an auto-generated name. Using tref.alias here
                // collided with CTE names ("WITH t AS (SELECT ... FROM
                // 'file.csv' AS t) ..." would try to create catalog table
                // 't' twice — once for the file ref, once for the CTE).
                std::string tbl_name =
                    "__nested_file_" + std::to_string(++preproc_uid) + "__";

                if (ext == "csv" || ext == "tsv") {
                    char delim = (ext == "tsv") ? '\t' : ',';
                    FastCSVReader reader(file_path, delim);
                    auto header = reader.ReadHeader();
                    auto types = reader.DetectTypes();
                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < header.size() && i < types.size(); i++)
                        cols.emplace_back(header[i], types[i]);
                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(types);
                    entry.SetStorage(storage);
                    entry.SetFilePath(file_path, delim);
                    temp_tables.push_back(tbl_name);
                } else if (ext == "parquet") {
                    ParquetReader reader(file_path);
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();
                    auto rows = reader.ReadAll();
                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);
                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    BulkLoadRows(*storage, col_types, rows);
                    temp_tables.push_back(tbl_name);
                } else if (ext == "json" || ext == "ndjson" || ext == "jsonl") {
                    JSONReader reader(file_path);
                    reader.DetectSchema();
                    auto rows = reader.ReadAll();
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();
                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);
                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    BulkLoadRows(*storage, col_types, rows);
                    temp_tables.push_back(tbl_name);
                } else {
                    // Leave other formats alone — top-level processing has
                    // dedicated handlers and nested cases for those formats
                    // aren't in the failing test set yet.
                    return;
                }

                tref.table_name = tbl_name;
                tref.is_table_function = false;
            };

            std::function<void(ParsedExpression *)> walk_expr_for_files;
            std::function<void(SelectStatement &)> walk_select_for_files;

            walk_expr_for_files = [&](ParsedExpression *e) {
                if (!e) return;
                using ET = ExpressionType;
                switch (e->GetExpressionType()) {
                case ET::SUBQUERY: {
                    auto *sq = static_cast<SubqueryExpression *>(e);
                    if (sq->subquery) {
                        // Materialise the subquery's FROM tree (file refs
                        // inside (SELECT ... FROM 'file.csv')), then
                        // recurse for any deeper subqueries / CTEs.
                        for (auto *n = sq->subquery->from_table.get(); n; n = n->right.get()) {
                            materialize_nested_file(*n);
                        }
                        walk_select_for_files(*sq->subquery);
                    }
                    walk_expr_for_files(sq->child.get());
                    break;
                }
                case ET::FUNCTION: {
                    auto *fn = static_cast<FunctionExpression *>(e);
                    for (auto &a : fn->arguments) walk_expr_for_files(a.get());
                    break;
                }
                case ET::COMPARISON: {
                    auto *c = static_cast<ComparisonExpression *>(e);
                    walk_expr_for_files(c->left.get());
                    walk_expr_for_files(c->right.get());
                    break;
                }
                case ET::CONJUNCTION: {
                    auto *c = static_cast<ConjunctionExpression *>(e);
                    walk_expr_for_files(c->left.get());
                    walk_expr_for_files(c->right.get());
                    break;
                }
                case ET::NEGATION:
                    walk_expr_for_files(static_cast<NegationExpression *>(e)->child.get());
                    break;
                case ET::IS_NULL:
                    walk_expr_for_files(static_cast<IsNullExpression *>(e)->child.get());
                    break;
                case ET::ARITHMETIC: {
                    auto *a = static_cast<ArithmeticExpression *>(e);
                    walk_expr_for_files(a->left.get());
                    walk_expr_for_files(a->right.get());
                    break;
                }
                case ET::UNARY_MINUS:
                    walk_expr_for_files(static_cast<UnaryMinusExpression *>(e)->child.get());
                    break;
                case ET::CAST:
                    walk_expr_for_files(static_cast<CastExpression *>(e)->child.get());
                    break;
                case ET::WINDOW: {
                    auto *w = static_cast<WindowExpression *>(e);
                    for (auto &a : w->arguments) walk_expr_for_files(a.get());
                    for (auto &a : w->partition_by) walk_expr_for_files(a.get());
                    for (auto &o : w->order_by) walk_expr_for_files(o.expression.get());
                    break;
                }
                default: break;
                }
            };

            // Walk the FROM tree of a NESTED select. Materialises any
            // bare-string-literal file ref via the simple BulkLoad path
            // — fine for nested contexts because they're typically much
            // smaller than the outer query and only need correctness,
            // not the full streaming/pushdown machinery.
            auto walk_from_tree = [&](SelectStatement &s) {
                for (auto *node = s.from_table.get(); node; node = node->right.get()) {
                    materialize_nested_file(*node);
                }
            };

            walk_select_for_files = [&](SelectStatement &s) {
                // Skip the OUTER from_table — run_preprocess below handles
                // it via the streaming-aware code path. Walker only fixes
                // up nested file refs (inside CTEs, subqueries) that the
                // existing top-level handler can't see.
                for (auto &e : s.select_list) walk_expr_for_files(e.get());
                walk_expr_for_files(s.where_clause.get());
                for (auto &e : s.group_by) walk_expr_for_files(e.get());
                walk_expr_for_files(s.having_clause.get());
                walk_expr_for_files(s.qualify_clause.get());
                for (auto &o : s.order_by) walk_expr_for_files(o.expression.get());
                walk_expr_for_files(s.limit.get());
                walk_expr_for_files(s.offset.get());
                for (auto &cte : s.ctes) {
                    if (cte.query) {
                        walk_from_tree(*cte.query);
                        walk_select_for_files(*cte.query);
                    }
                }
                if (s.set_right) walk_select_for_files(*s.set_right);
            };
            // For SubqueryExpressions visited by walk_expr_for_files, the
            // SUBQUERY case there calls walk_select_for_files on the inner
            // SelectStatement — but we also need to materialise the inner
            // FROM tree first. Handled by patching walk_expr_for_files.

            walk_select_for_files(outer_sel);
            // ----------------------------------------------------------

            for (SelectStatement *cur_sel = &outer_sel; cur_sel;
                 cur_sel = cur_sel->set_right.get()) {
            auto &sel = *cur_sel;
            ++preproc_uid;

            // Wrap the whole preprocessing below in a zero-arg lambda so
            // we can re-run it for each TableRef in a JOIN chain. The
            // lambda reads/writes `sel.from_table`; after the main pass
            // we swap each JOIN right-hand-side into that slot in turn.
            // Without this, `FROM 'a.csv' JOIN 'b.csv' ON ...` leaves
            // the right side as __FILE__ and the binder errors out.
            auto run_preprocess = [&]() {

            // Expand virtual views in the FROM clause.
            auto expand_view = [&](TableRef &ref) {
                if (ref.is_table_function) return;
                auto *entry = db_.GetCatalog().GetTable(ref.table_name);
                if (!entry || !entry->IsView()) return;

                // Live views cache their result; decide below between cache
                // hit, incremental append, and full rescan based on the
                // watched file's size / leading bytes / mtime.
                if (entry->IsLiveView()) {
                    const auto &path = entry->GetWatchedFile();
                    std::error_code ec1, ec2;
                    auto t = std::filesystem::last_write_time(path, ec1);
                    auto sz = std::filesystem::file_size(path, ec2);
                    if (!ec1 && !ec2) {
                        int64_t m = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t.time_since_epoch()).count();
                        int64_t cur_size = static_cast<int64_t>(sz);

                        // Read the first 64 bytes as a "beginning unchanged"
                        // sentinel. Cheap; protects against rewrite-in-place
                        // where the size happens to be ≥ the previous size.
                        std::string current_head;
                        {
                            std::ifstream in(path, std::ios::binary);
                            char head[64] = {0};
                            in.read(head, sizeof(head));
                            current_head.assign(head, in.gcount());
                        }
                        bool header_changed = current_head != entry->GetFirstBytes();
                        int64_t last_size = entry->GetLastSize();

                        if (entry->HasCache() && !header_changed &&
                            cur_size == last_size) {
                            return; // cache hit
                        }

                        // v2 incremental-append path: CSV file grew, leading
                        // bytes unchanged, and the view is the pass-through
                        // shape marked at CREATE time.
                        if (entry->HasCache() && !header_changed &&
                            cur_size > last_size &&
                            entry->IsIncrementalEligible()) {
                            auto types = entry->GetTypes();
                            char delim = entry->GetIncrementalDelim();
                            // Mmap the file via FastCSVReader, then reposition
                            // past any partial tail line at the append seam.
                            FastCSVReader reader(path, delim,
                                /*has_header=*/false);
                            size_t seek = FastCSVReader::FindLineStart(
                                reader.GetBuffer(), reader.GetSize(),
                                static_cast<size_t>(last_size));
                            reader.SetPos(seek);
                            DataChunk chunk;
                            chunk.Initialize(types);
                            while (true) {
                                chunk.Reset();
                                idx_t n = reader.ReadChunk(chunk, types);
                                if (n == 0) break;
                                entry->GetStorage().Append(chunk);
                            }
                            entry->SetLastSize(cur_size);
                            entry->SetCachedMTime(m);
                            return;
                        }

                        // Fall through to full rescan. Update all cached
                        // fields after re-execution below.
                        entry->SetCachedMTime(m);
                        entry->SetLastSize(cur_size);
                        entry->SetFirstBytes(current_head);
                    }
                }
                // Re-execute the view's stored query to get fresh data.
                // SELECT now collects results into `chunks` (avoids per-cell
                // boxing on the hot path). Materialise once for BulkLoadRows.
                auto view_result = Query(entry->GetViewQuery());
                view_result.MaterialiseRows();
                auto storage = std::make_shared<DataTable>(view_result.column_types);
                BulkLoadRows(*storage, view_result.column_types, view_result.rows);
                entry->SetStorage(storage);
                if (entry->IsLiveView()) entry->MarkCacheValid();
            };

            if (sel.from_table && !sel.from_table->is_table_function) {
                expand_view(*sel.from_table);
                if (sel.from_table->right) expand_view(*sel.from_table->right);
            }
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "GENERATE_SERIES") {
                // Evaluate args as constants. SQL standard variants:
                //   generate_series(stop)              — start=0, step=1
                //   generate_series(start, stop)       — step=1
                //   generate_series(start, stop, step)
                // Prior code only handled the 2+ arg forms and silently
                // left start=stop=0 for the 1-arg form, producing an
                // empty result — fix as a general engine improvement.
                int64_t start_val = 0, stop_val = 0, step_val = 1;
                auto &fa = sel.from_table->function_args;
                if (fa.size() == 1) {
                    auto &e = static_cast<ConstantExpression &>(*fa[0]);
                    stop_val = std::stoll(e.value);
                } else if (fa.size() >= 2) {
                    auto &s = static_cast<ConstantExpression &>(*fa[0]);
                    start_val = std::stoll(s.value);
                    auto &e = static_cast<ConstantExpression &>(*fa[1]);
                    stop_val = std::stoll(e.value);
                }
                if (fa.size() >= 3) {
                    auto &st = static_cast<ConstantExpression &>(*fa[2]);
                    step_val = std::stoll(st.value);
                }

                std::string tbl_name = sel.from_table->alias.empty()
                    ? ("__generate_series_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                // SQL standard column-alias list: AS t(v1) renames the
                // single output column. Without one, the column name is
                // the function name ("generate_series").
                std::string col_name = sel.from_table->column_aliases.empty()
                    ? "generate_series" : sel.from_table->column_aliases[0];
                if (sel.from_table->column_aliases.size() > 1) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "GENERATE_SERIES produces one column but column-alias list has " +
                        std::to_string(sel.from_table->column_aliases.size()));
                }
                sel.from_table->table_name = tbl_name;
                sel.from_table->is_table_function = false;

                if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                auto &entry = db_.GetCatalog().CreateTable(tbl_name,
                    {ColumnDefinition(col_name, LogicalType::BIGINT())});
                auto storage = std::make_shared<DataTable>(
                    std::vector<LogicalType>{LogicalType::BIGINT()});
                entry.SetStorage(storage);

                if (step_val == 0) throw InvalidInputException("GENERATE_SERIES step cannot be 0");
                static constexpr int64_t MAX_SERIES_ROWS = 10000000; // 10M limit
                int64_t est_rows = (step_val > 0) ? (stop_val - start_val) / step_val + 1 : 0;
                if (est_rows > MAX_SERIES_ROWS || est_rows < 0)
                    throw InvalidInputException("GENERATE_SERIES would produce too many rows (limit: 10M)");

                for (int64_t v = start_val;
                     (step_val > 0) ? (v <= stop_val) : (v >= stop_val);
                     v += step_val) {
                    DataChunk chunk;
                    chunk.Initialize({LogicalType::BIGINT()});
                    chunk.SetValue(0, 0, Value::BIGINT(v));
                    storage->Append(chunk);
                }
                temp_tables.push_back(tbl_name);
            }

            // Helper: process a TableRef that's a read_csv() call.
            auto process_read_csv = [&](TableRef &tref) -> bool {
                if (!tref.is_table_function) return false;
                auto upper = StringUtil::Upper(tref.table_name);
                if (upper != "READ_CSV" && upper != "READ_CSV_AUTO") return false;
                auto &args = tref.function_args;
                if (args.empty() || args[0]->GetExpressionType() != ExpressionType::CONSTANT) return false;

                auto file_pattern = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);
                auto files = FileGlob::Glob(file_pattern);
                if (files.empty()) throw IOException("No files matching: " + file_pattern);

                FastCSVReader reader(files[0], ',', true, 65536);
                auto header = reader.ReadHeader();
                auto types = reader.DetectTypes();

                static int unique_id = 0;
                std::string tbl_name = tref.alias.empty()
                    ? ("__read_csv_" + std::to_string(++unique_id) + "__") : tref.alias;
                tref.table_name = tbl_name;
                tref.is_table_function = false;

                // SQL standard column-alias list overrides detected header
                // names positionally: read_csv('f.csv') AS t(c1, c2) renames
                // the first two columns to c1, c2. If the alias list has a
                // different count than the source produces, reject — that's
                // a user error worth surfacing.
                if (!tref.column_aliases.empty() &&
                    tref.column_aliases.size() != types.size()) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "read_csv column-alias list has " +
                        std::to_string(tref.column_aliases.size()) +
                        " columns but the CSV has " +
                        std::to_string(types.size()));
                }
                std::vector<ColumnDefinition> cols;
                for (size_t i = 0; i < header.size() && i < types.size(); i++) {
                    const std::string &name = (i < tref.column_aliases.size())
                                                ? tref.column_aliases[i]
                                                : header[i];
                    cols.emplace_back(name, types[i]);
                }

                if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                auto storage = std::make_shared<DataTable>(types);
                entry.SetStorage(storage);
                entry.SetFilePath(files[0]);
                temp_tables.push_back(tbl_name);
                return true;
            };

            // Apply to from_table and any joined table on the right.
            if (sel.from_table) {
                process_read_csv(*sel.from_table);
                if (sel.from_table->right) process_read_csv(*sel.from_table->right);
            }

            // Original handler kept for compatibility (still used for json/parquet/etc).
            if (sel.from_table && sel.from_table->is_table_function &&
                (StringUtil::Upper(sel.from_table->table_name) == "READ_CSV" ||
                 StringUtil::Upper(sel.from_table->table_name) == "READ_CSV_AUTO")) {
                // (no-op now; processed above)
                if (false) {
                    std::vector<ColumnDefinition> cols;
                }
            }

            // Handle read_json / read_json_auto table function.
            if (sel.from_table && sel.from_table->is_table_function &&
                (StringUtil::Upper(sel.from_table->table_name) == "READ_JSON" ||
                 StringUtil::Upper(sel.from_table->table_name) == "READ_JSON_AUTO")) {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto file_path = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);

                    JSONReader reader(file_path);
                    reader.DetectSchemaLight();
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? ("__read_json_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);

                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    // Placeholder storage (unused by PhysicalJSONScan) - keeps
                    // catalog invariants for other code that calls GetStorage().
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    // Mark as JSON file-backed so PhysicalPlanner::PlanGet
                    // dispatches to PhysicalJSONScan instead of bulk-loading.
                    entry.SetJsonPath(file_path);
                    temp_tables.push_back(tbl_name);
                }
            }

            // Handle read_parquet table function - schema only; scan is streamed
            // at execution time via PhysicalParquetScan.
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_PARQUET") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto raw_path = static_cast<ConstantExpression &>(*args[0]).value;
                    bool has_glob = raw_path.find('*') != std::string::npos ||
                                    raw_path.find('?') != std::string::npos;

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? ("__read_parquet_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    if (has_glob) {
                        // Multi-file glob: PhysicalParquetScan only handles
                        // a single file, so we bulk-load all matches into
                        // an in-memory DataTable and let PhysicalTableScan
                        // serve the read. Schema is taken from the first
                        // file; subsequent files must have the same column
                        // count.
                        auto files = FileGlob::Glob(raw_path);
                        if (files.empty()) throw IOException("No files matching: " + raw_path);
                        ParquetReader first(files[0]);
                        auto col_names = first.GetColumnNames();
                        auto col_types = first.GetColumnTypes();

                        std::vector<ColumnDefinition> cols;
                        for (size_t i = 0; i < col_names.size(); i++)
                            cols.emplace_back(col_names[i], col_types[i]);
                        if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                        auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                        auto storage = std::make_shared<DataTable>(col_types);
                        entry.SetStorage(storage);

                        BulkLoadRows(*storage, col_types, first.ReadAll());
                        for (size_t i = 1; i < files.size(); i++) {
                            ParquetReader r(files[i]);
                            if (r.GetColumnTypes().size() != col_types.size())
                                throw IOException("Schema mismatch in glob: " +
                                    files[i] + " column count differs from " + files[0]);
                            BulkLoadRows(*storage, col_types, r.ReadAll());
                        }
                        temp_tables.push_back(tbl_name);
                    } else {
                        auto file_path = ResolveRemoteFile(raw_path);

                        // Open just to read the footer metadata (cheap). We stash
                        // the reader on the catalog entry so PhysicalParquetScan
                        // can reuse it instead of re-parsing the Thrift footer at
                        // execution time - this path is hit once per query and
                        // saves ~10-20ms per query.
                        auto reader_sp = std::make_shared<ParquetReader>(file_path);
                        auto col_names = reader_sp->GetColumnNames();
                        auto col_types = reader_sp->GetColumnTypes();

                        std::vector<ColumnDefinition> cols;
                        for (size_t i = 0; i < col_names.size(); i++)
                            cols.emplace_back(col_names[i], col_types[i]);

                        if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                        auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                        auto storage = std::make_shared<DataTable>(col_types);
                        entry.SetStorage(storage);
                        entry.SetParquetPath(file_path);
                        entry.SetCachedParquetReader(std::move(reader_sp));
                        temp_tables.push_back(tbl_name);
                    }
                }
            }

            // Handle read_arrow table function.
#ifdef SLOTHDB_EDGE
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_ARROW") {
                throw BinderException("read_arrow is unavailable in SlothDB "
                    "edge build - use the full build (@slothdb/wasm) for "
                    "Arrow IPC support");
            }
#else
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_ARROW") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto fp = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);
                    // Schema-only parse - body gets streamed by
                    // PhysicalArrowScan at execution time.
                    ArrowIPCReader reader(fp);
                    reader.DetectSchemaLight();
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? ("__read_arrow_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);
                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    entry.SetArrowPath(fp);
                    temp_tables.push_back(tbl_name);
                }
            }
#endif // SLOTHDB_EDGE

            // Handle read_avro table function.
#ifdef SLOTHDB_EDGE
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_AVRO") {
                throw BinderException("read_avro is unavailable in SlothDB "
                    "edge build - use the full build (@slothdb/wasm) for "
                    "Avro support");
            }
#else
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_AVRO") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto fp = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);
                    AvroReader reader(fp);
                    reader.DetectSchemaLight();
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? ("__read_avro_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);
                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    // PhysicalAvroScan parses at execution time.
                    entry.SetAvroPath(fp);
                    temp_tables.push_back(tbl_name);
                }
            }
#endif // SLOTHDB_EDGE

            // Handle read_xlsx table function.
#ifdef SLOTHDB_EDGE
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_XLSX") {
                throw BinderException("read_xlsx is unavailable in SlothDB "
                    "edge build - use the full build (@slothdb/wasm) for "
                    "Excel support");
            }
#else
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_XLSX") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto fp = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);
                    ExcelReader reader(fp);
                    // ReadAll must run first - it is what actually parses the
                    // xlsx and populates column_names_/column_types_.
                    auto rows = reader.ReadAll();
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? ("__read_xlsx_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);
                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    BulkLoadRows(*storage, col_types, rows);
                    temp_tables.push_back(tbl_name);
                }
            }
#endif // SLOTHDB_EDGE

            // Handle sqlite_scan table function.
#ifdef SLOTHDB_EDGE
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "SQLITE_SCAN") {
                throw BinderException("sqlite_scan is unavailable in SlothDB "
                    "edge build - use the full build (@slothdb/wasm) for "
                    "SQLite support");
            }
#else
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "SQLITE_SCAN") {
                auto &args = sel.from_table->function_args;
                if (args.size() >= 2 &&
                    args[0]->GetExpressionType() == ExpressionType::CONSTANT &&
                    args[1]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto db_path = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);
                    auto table_name = static_cast<ConstantExpression &>(*args[1]).value;

                    // Schema-only probe - body gets streamed at execution
                    // time by PhysicalSQLiteScan.
                    SQLiteScanner scanner(db_path);
                    auto col_info = scanner.GetColumns(table_name);

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? ("__sqlite_scan_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    std::vector<LogicalType> col_types;
                    for (auto &ci : col_info) {
                        cols.emplace_back(ci.name, ci.type);
                        col_types.push_back(ci.type);
                    }

                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    entry.SetSQLitePath(db_path, table_name);
                    temp_tables.push_back(tbl_name);
                }
            }
#endif // SLOTHDB_EDGE

            // Handle auto-detect: SELECT * FROM 'file.ext'
            if (sel.from_table && sel.from_table->is_table_function &&
                sel.from_table->table_name == "__FILE__") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto file_path = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);

                    // Detect format from extension.
                    auto ext = file_path.substr(file_path.find_last_of('.') + 1);
                    for (auto &c : ext) c = static_cast<char>(std::tolower(c));

                    if (ext == "csv" || ext == "tsv") {
                        sel.from_table->table_name = "READ_CSV";
                    } else if (ext == "json" || ext == "ndjson" || ext == "jsonl") {
                        sel.from_table->table_name = "READ_JSON";
                    } else if (ext == "parquet") {
                        sel.from_table->table_name = "READ_PARQUET";
#ifndef SLOTHDB_EDGE
                    } else if (ext == "arrow" || ext == "feather" || ext == "ipc") {
                        sel.from_table->table_name = "READ_ARROW";
                    } else if (ext == "avro") {
                        sel.from_table->table_name = "READ_AVRO";
                    } else if (ext == "xlsx" || ext == "xls") {
                        sel.from_table->table_name = "READ_XLSX";
                    } else if (ext == "db" || ext == "sqlite" || ext == "sqlite3") {
                        sel.from_table->table_name = "SQLITE_SCAN";
#endif
                    } else {
                        throw IOException("Unknown file format: ." + ext
#ifdef SLOTHDB_EDGE
                            + " (edge build supports CSV / JSON / Parquet only)"
#endif
                        );
                    }
                    // Re-process with the detected table function name.
                    // (The loop will pick it up on the next iteration - but we're
                    //  in the same iteration. Let's handle inline.)
                    if (sel.from_table->table_name == "READ_CSV") {
                        char delim = (ext == "tsv") ? '\t' : ',';
                        FastCSVReader reader(file_path, delim);
                        auto header = reader.ReadHeader();
                        auto types = reader.DetectTypes();

                        std::string tbl_name = sel.from_table->alias.empty()
                            ? ("__auto_file_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                        sel.from_table->table_name = tbl_name;
                        sel.from_table->is_table_function = false;

                        std::vector<ColumnDefinition> cols;
                        for (size_t i = 0; i < header.size() && i < types.size(); i++)
                            cols.emplace_back(header[i], types[i]);
                        if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                        auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                        auto storage = std::make_shared<DataTable>(types);
                        entry.SetStorage(storage);
                        entry.SetFilePath(file_path, delim);
                        temp_tables.push_back(tbl_name);
                    } else if (sel.from_table->table_name == "READ_JSON") {
                        JSONReader reader(file_path);
                        reader.DetectSchema();
                        auto rows = reader.ReadAll();
                        auto col_names = reader.GetColumnNames();
                        auto col_types = reader.GetColumnTypes();

                        std::string tbl_name = sel.from_table->alias.empty()
                            ? ("__auto_file_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                        sel.from_table->table_name = tbl_name;
                        sel.from_table->is_table_function = false;

                        std::vector<ColumnDefinition> cols;
                        for (size_t i = 0; i < col_names.size(); i++)
                            cols.emplace_back(col_names[i], col_types[i]);
                        if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                        auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                        auto storage = std::make_shared<DataTable>(col_types);
                        entry.SetStorage(storage);
                        BulkLoadRows(*storage, col_types, rows);
                        temp_tables.push_back(tbl_name);
                    } else if (sel.from_table->table_name == "READ_PARQUET") {
                        bool has_glob = file_path.find('*') != std::string::npos ||
                                        file_path.find('?') != std::string::npos;
                        std::string tbl_name = sel.from_table->alias.empty()
                            ? ("__auto_file_" + std::to_string(preproc_uid) + "__") : sel.from_table->alias;
                        sel.from_table->table_name = tbl_name;
                        sel.from_table->is_table_function = false;

                        if (has_glob) {
                            auto files = FileGlob::Glob(file_path);
                            if (files.empty()) throw IOException("No files matching: " + file_path);
                            ParquetReader first(files[0]);
                            auto col_names = first.GetColumnNames();
                            auto col_types = first.GetColumnTypes();
                            std::vector<ColumnDefinition> cols;
                            for (size_t i = 0; i < col_names.size(); i++)
                                cols.emplace_back(col_names[i], col_types[i]);
                            if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                            auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                            auto storage = std::make_shared<DataTable>(col_types);
                            entry.SetStorage(storage);
                            BulkLoadRows(*storage, col_types, first.ReadAll());
                            for (size_t i = 1; i < files.size(); i++) {
                                ParquetReader r(files[i]);
                                if (r.GetColumnTypes().size() != col_types.size())
                                    throw IOException("Schema mismatch in glob: " +
                                        files[i] + " column count differs from " + files[0]);
                                BulkLoadRows(*storage, col_types, r.ReadAll());
                            }
                            temp_tables.push_back(tbl_name);
                        } else {
                            // Use the same streaming setup as the explicit
                            // read_parquet() table-function form: parse the
                            // footer to get the schema, stash a shared reader
                            // on the catalog entry so PhysicalParquetScan can
                            // stream column chunks at execution time.
                            // BulkLoadRows would materialise the entire file
                            // into memory — way slower than streaming, and
                            // for HTTPS URLs that's a 132s vs 5s gap.
                            auto reader_sp = std::make_shared<ParquetReader>(file_path);
                            auto col_names = reader_sp->GetColumnNames();
                            auto col_types = reader_sp->GetColumnTypes();

                            std::vector<ColumnDefinition> cols;
                            for (size_t i = 0; i < col_names.size(); i++)
                                cols.emplace_back(col_names[i], col_types[i]);
                            if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                            auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                            auto storage = std::make_shared<DataTable>(col_types);
                            entry.SetStorage(storage);
                            entry.SetParquetPath(file_path);
                            entry.SetCachedParquetReader(std::move(reader_sp));
                            temp_tables.push_back(tbl_name);
                        }
                    }
                }
            }
            }; // end run_preprocess lambda

            // Pass 1: process the original sel.from_table (the JOIN root).
            run_preprocess();

            // Pass 2+: walk the JOIN right-hand chain. For each parent
            // whose `->right` is a file-literal / read_xxx / __FILE__,
            // detach it, swap into sel.from_table, rerun the same
            // preprocessing, swap back. We process deepest-first so
            // each detach-swap-reattach cycle is independent.
            std::vector<TableRef*> join_parents;
            for (auto *node = sel.from_table.get();
                 node && node->right; node = node->right.get()) {
                join_parents.push_back(node);
            }
            for (auto it = join_parents.rbegin(); it != join_parents.rend(); ++it) {
                TableRef *parent = *it;
                auto rhs = std::move(parent->right);    // detach
                std::swap(sel.from_table, rhs);         // RHS is now the active from_table
                run_preprocess();
                std::swap(sel.from_table, rhs);         // put root back
                parent->right = std::move(rhs);         // reattach mutated RHS
            }
            } // end for-loop over set_right chain
        }

        // Handle CTEs: materialize each CTE as a temporary table.
        // (cte_tables / _cte_temp_tables are declared at the top of this
        //  iteration body and cleaned up by TableCleanupGuard.)

        // Helper to expand read_csv() in any TableRef.
        auto cte_process_read_csv = [&](TableRef &tref) -> bool {
            if (!tref.is_table_function) return false;
            auto upper = StringUtil::Upper(tref.table_name);
            if (upper != "READ_CSV" && upper != "READ_CSV_AUTO") return false;
            auto &args = tref.function_args;
            if (args.empty() || args[0]->GetExpressionType() != ExpressionType::CONSTANT) return false;

            auto file_pattern = ResolveRemoteFile(static_cast<ConstantExpression &>(*args[0]).value);
            auto files = FileGlob::Glob(file_pattern);
            if (files.empty()) throw IOException("No files matching: " + file_pattern);

            FastCSVReader reader(files[0], ',', true, 65536);
            auto header = reader.ReadHeader();
            auto types = reader.DetectTypes();

            static int cte_unique_id = 0;
            std::string tbl_name = tref.alias.empty()
                ? ("__cte_read_csv_" + std::to_string(++cte_unique_id) + "__") : tref.alias;
            tref.table_name = tbl_name;
            tref.is_table_function = false;

            std::vector<ColumnDefinition> cols;
            for (size_t i = 0; i < header.size() && i < types.size(); i++)
                cols.emplace_back(header[i], types[i]);

            auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
            auto storage = std::make_shared<DataTable>(types);
            entry.SetStorage(storage);
            entry.SetFilePath(files[0]);
            _cte_temp_tables.push_back(tbl_name);
            return true;
        };

        // Materialise a SelectStatement into a freshly-created catalog
        // table, returning its storage. Used by the subquery-in-FROM and
        // non-recursive CTE paths.
        //
        // Fast path (no set_op chain): drains physical chunks straight to
        // storage. Identical cost to the pre-bugfix CTE code.
        //
        // Slow path (UNION / UNION ALL / INTERSECT / EXCEPT chain): walks
        // the right-leaning chain produced by the parser, binds every
        // segment to discover its result types, computes a per-column
        // common type across all branches (so INT + BIGINT lands in
        // BIGINT instead of overflowing the INT storage), then drains
        // every branch into rows, coerces each Value to the common type,
        // applies set semantics (UNION / INTERSECT / EXCEPT use
        // type-aware Value equality so INT 1 doesn't collide with
        // VARCHAR "1" the way the previous string-key dedup did), and
        // writes the combined rows to storage with the common types.
        // The planner has no logical UNION node — set_op is walked at the
        // Connection layer.
        auto materialize_select_into_new_table = [&](
                SelectStatement &sel,
                const std::string &dest_name) -> std::shared_ptr<DataTable> {
            Binder lb(db_.GetCatalog());
            auto lbound = lb.Bind(sel);
            auto &lsel = static_cast<BoundSelectStatement &>(*lbound);

            // Fast path: no set_op chain — use LEFT types directly, drain
            // chunks straight to storage.
            if (sel.set_op.empty()) {
                std::vector<ColumnDefinition> cols;
                for (idx_t i = 0; i < lsel.result_names.size(); i++)
                    cols.emplace_back(lsel.result_names[i], lsel.result_types[i]);
                auto &entry = db_.GetCatalog().CreateTable(dest_name, std::move(cols));
                auto storage = std::make_shared<DataTable>(lsel.result_types);
                entry.SetStorage(storage);

                auto llogical = Planner::Plan(*lbound);
                llogical = Optimizer::Optimize(std::move(llogical));
                PhysicalPlanner lpp(db_.GetCatalog());
                auto lphysical = lpp.Plan(*llogical);
                lphysical->Init();
                DataChunk lchunk;
                while (true) {
                    if (!lphysical->GetData(lchunk)) break;
                    storage->Append(lchunk);
                }
                return storage;
            }

            // Slow path. First pass: walk the set_op chain and bind every
            // branch so we know each branch's types before any execution.
            std::vector<std::unique_ptr<BoundStatement>> branch_bounds;
            branch_bounds.push_back(std::move(lbound));
            {
                SelectStatement *cur = &sel;
                while (!cur->set_op.empty() && cur->set_right) {
                    Binder rb(db_.GetCatalog());
                    branch_bounds.push_back(rb.Bind(*cur->set_right));
                    cur = cur->set_right.get();
                }
            }

            // Per-column common type via pairwise reduction across branches.
            // Names come from the LEFT branch (SQL standard).
            std::vector<LogicalType> result_types;
            std::vector<std::string> result_names;
            {
                auto &first = static_cast<BoundSelectStatement &>(*branch_bounds[0]);
                result_types = first.result_types;
                result_names = first.result_names;
                for (size_t b = 1; b < branch_bounds.size(); b++) {
                    auto &bs = static_cast<BoundSelectStatement &>(*branch_bounds[b]);
                    for (idx_t c = 0; c < result_types.size() && c < bs.result_types.size(); c++) {
                        result_types[c] = ComputeSetOpCommonType(result_types[c], bs.result_types[c]);
                    }
                }
            }

            // Drain a freshly-planned branch into row vectors, coercing
            // each Value to the corresponding common type.
            auto drain_branch_coerced = [&](BoundStatement &bound,
                                             std::vector<std::vector<Value>> &out_rows) {
                auto logical = Planner::Plan(bound);
                logical = Optimizer::Optimize(std::move(logical));
                PhysicalPlanner pp(db_.GetCatalog());
                auto physical = pp.Plan(*logical);
                physical->Init();
                DataChunk chunk;
                while (true) {
                    if (!physical->GetData(chunk)) break;
                    for (idx_t i = 0; i < chunk.size(); i++) {
                        std::vector<Value> row;
                        row.reserve(chunk.ColumnCount());
                        for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
                            row.push_back(CoerceValueForSetOp(
                                chunk.GetValue(c, i), result_types[c]));
                        }
                        out_rows.push_back(std::move(row));
                    }
                }
            };

            // LEFT branch into rows.
            std::vector<std::vector<Value>> rows;
            drain_branch_coerced(*branch_bounds[0], rows);

            // Walk the chain applying set semantics with the new common types.
            {
                SelectStatement *cur = &sel;
                size_t branch_idx = 1;
                while (!cur->set_op.empty() && cur->set_right) {
                    std::vector<std::vector<Value>> right_rows;
                    drain_branch_coerced(*branch_bounds[branch_idx++], right_rows);

                    const std::string &op = cur->set_op;
                    if (op == "UNION ALL") {
                        for (auto &r : right_rows) rows.push_back(std::move(r));
                    } else if (op == "UNION") {
                        SetOpRowSet seen;
                        for (auto &row : rows) seen.insert(row);
                        for (auto &row : right_rows) {
                            if (seen.insert(row).second)
                                rows.push_back(std::move(row));
                        }
                    } else if (op == "INTERSECT") {
                        SetOpRowSet rk(right_rows.begin(), right_rows.end());
                        std::vector<std::vector<Value>> kept;
                        for (auto &row : rows) {
                            if (rk.count(row)) kept.push_back(std::move(row));
                        }
                        rows = std::move(kept);
                    } else if (op == "EXCEPT") {
                        SetOpRowSet rk(right_rows.begin(), right_rows.end());
                        std::vector<std::vector<Value>> kept;
                        for (auto &row : rows) {
                            if (!rk.count(row)) kept.push_back(std::move(row));
                        }
                        rows = std::move(kept);
                    }
                    cur = cur->set_right.get();
                }
            }

            // Create storage with the common types and write rows.
            std::vector<ColumnDefinition> cols;
            for (idx_t i = 0; i < result_names.size(); i++)
                cols.emplace_back(result_names[i], result_types[i]);
            auto &entry = db_.GetCatalog().CreateTable(dest_name, std::move(cols));
            auto storage = std::make_shared<DataTable>(result_types);
            entry.SetStorage(storage);
            BulkLoadRows(*storage, result_types, rows);
            return storage;
        };

        // Materialize FROM-clause subqueries — `FROM (SELECT ...) AS s` —
        // into temp tables before binding. Recurses bottom-up so nested
        // subqueries are materialized first. Subquery becomes a regular
        // catalog table named `__subq_N__`; the TableRef keeps the
        // user-provided alias so column refs like `s.a` resolve normally.
        std::function<void(TableRef &)> materialize_from_subqueries =
                [&](TableRef &tref) {
            if (tref.subquery) {
                if (tref.subquery->from_table)
                    materialize_from_subqueries(*tref.subquery->from_table);
                for (auto &cte : tref.subquery->ctes) {
                    if (cte.query && cte.query->from_table)
                        materialize_from_subqueries(*cte.query->from_table);
                }
                static int subq_unique_id = 0;
                std::string subq_name = "__subq_" +
                        std::to_string(++subq_unique_id) + "__";

                materialize_select_into_new_table(*tref.subquery, subq_name);
                cte_tables.push_back(subq_name);

                tref.table_name = subq_name;
                tref.subquery.reset();
            }
            if (tref.right) materialize_from_subqueries(*tref.right);
        };

        if (stmt->GetType() == StatementType::SELECT) {
            auto &sel0 = static_cast<SelectStatement &>(*stmt);
            if (sel0.from_table) materialize_from_subqueries(*sel0.from_table);
            for (auto &cte : sel0.ctes) {
                if (cte.query && cte.query->from_table)
                    materialize_from_subqueries(*cte.query->from_table);
            }
        }

        if (stmt->GetType() == StatementType::SELECT) {
            auto &sel = static_cast<SelectStatement &>(*stmt);
            for (auto &cte : sel.ctes) {
                if (cte.recursive && cte.query->set_right) {
                    // Recursive CTE: execute base case, then iterate.
                    // Base case is cte.query (left side of UNION ALL).
                    // Recursive case is cte.query->set_right.

                    // 1. Execute base case.
                    Binder base_binder(db_.GetCatalog());
                    auto base_bound = base_binder.Bind(*cte.query);
                    auto &base_sel = static_cast<BoundSelectStatement &>(*base_bound);

                    std::vector<ColumnDefinition> cte_cols;
                    for (idx_t i = 0; i < base_sel.result_names.size(); i++)
                        cte_cols.emplace_back(base_sel.result_names[i], base_sel.result_types[i]);

                    auto &entry = db_.GetCatalog().CreateTable(cte.name, cte_cols);
                    // Push for cleanup BEFORE the iteration runs. If binding
                    // or planning the recursive case throws, the table still
                    // gets dropped by the TableCleanupGuard on this query's
                    // exit. Without this, a failed recursive CTE leaves the
                    // catalog table in place and the next query that uses
                    // the same CTE name fails with "Table already exists".
                    cte_tables.push_back(cte.name);
                    auto storage = std::make_shared<DataTable>(base_sel.result_types);
                    entry.SetStorage(storage);

                    auto base_logical = Planner::Plan(*base_bound);
                    PhysicalPlanner base_pp(db_.GetCatalog());
                    auto base_physical = base_pp.Plan(*base_logical);
                    base_physical->Init();
                    DataChunk cte_chunk;
                    while (true) {
                        if (!base_physical->GetData(cte_chunk)) break;
                        storage->Append(cte_chunk);
                    }

                    // 2. Iterate recursive case (max 1000 iterations, 10M total rows).
                    static constexpr idx_t MAX_RECURSIVE_ROWS = 10000000;
                    for (int iter = 0; iter < 1000; iter++) {
                        idx_t prev_count = storage->Count();
                        if (prev_count > MAX_RECURSIVE_ROWS)
                            throw InternalException("Recursive CTE exceeded 10M row limit");

                        Binder rec_binder(db_.GetCatalog());
                        auto rec_bound = rec_binder.Bind(*cte.query->set_right);
                        auto rec_logical = Planner::Plan(*rec_bound);
                        PhysicalPlanner rec_pp(db_.GetCatalog());
                        auto rec_physical = rec_pp.Plan(*rec_logical);
                        rec_physical->Init();

                        DataChunk rec_chunk;
                        bool got_rows = false;
                        while (true) {
                            if (!rec_physical->GetData(rec_chunk)) break;
                            if (rec_chunk.size() > 0) {
                                storage->Append(rec_chunk);
                                got_rows = true;
                            }
                        }

                        if (!got_rows || storage->Count() == prev_count) break;
                    }
                    // (cte_tables.push_back already happened right after
                    //  CreateTable above so a mid-iteration throw still cleans up.)
                } else {
                    // Non-recursive CTE - pre-process read_csv in the inner SELECT.
                    auto cte_stmt = std::make_unique<SelectStatement>();
                    *cte_stmt = std::move(*cte.query);

                    // Apply read_csv expansion to CTE's from_table (and any join right side).
                    if (cte_stmt->from_table) {
                        cte_process_read_csv(*cte_stmt->from_table);
                        if (cte_stmt->from_table->right) cte_process_read_csv(*cte_stmt->from_table->right);
                    }

                    // Helper drains the full set_op chain when present;
                    // otherwise takes the chunk-direct fast path.
                    materialize_select_into_new_table(*cte_stmt, cte.name);
                    cte_tables.push_back(cte.name);
                }
            }
        }

        // 2. Bind.
        Binder binder(db_.GetCatalog());
        auto bound = binder.Bind(*stmt);

        // Capture per-result-column DISPLAY types now — Planner::Plan below
        // moves the bound select-list out of the statement, so the date/
        // timestamp re-tag (applied to the final result further down) must
        // read the select-list expressions here while they still exist.
        std::vector<LogicalType> result_display_types;
        if (bound->GetType() == BoundStatementType::SELECT) {
            auto &sel = static_cast<BoundSelectStatement &>(*bound);
            result_display_types.reserve(sel.select_list.size());
            for (auto &e : sel.select_list) {
                result_display_types.push_back(
                    e ? ResolveResultDisplayType(*e, sel.table)
                      : LogicalType(LogicalTypeId::INVALID));
            }
        }

        // 3. Logical plan.
        auto logical = Planner::Plan(*bound);

        // 3b. Optimize logical plan.
        logical = Optimizer::Optimize(std::move(logical));

        // 4. Physical plan.
        PhysicalPlanner physical_planner(db_.GetCatalog());
        auto physical = physical_planner.Plan(*logical);

        // 5. Execute.
        ExpressionExecutor::SetCatalog(&db_.GetCatalog());
        // Kick off column-projection pushdown from the root: every result
        // column is needed, intermediate operators expand to include any
        // column they reference (filter predicates, sort keys, agg args)
        // and forward to their children. Cuts decode work to just the
        // referenced columns at the parquet scan boundary instead of all.
        {
            std::vector<bool> top_mask(physical->GetTypes().size(), true);
            physical->SetNeededOutputs(top_mask);
        }
        physical->Init();

        // Collect result column names/types from bound statement.
        if (bound->GetType() == BoundStatementType::SELECT) {
            auto &sel = static_cast<BoundSelectStatement &>(*bound);
            final_result.column_names = sel.result_names;
            final_result.column_types = sel.result_types;
        }

        // Stream chunks directly into the result, avoiding per-cell Value
        // boxing. The C API's typed-batch fetch (slothdb_column_*_buffer)
        // reads from `chunks` directly; the per-cell path goes through
        // QueryResult::GetValue which lazily boxes on demand.
        // 10M-row window query: drops 18 s of cell-boxing.
        DataChunk chunk;
        while (true) {
            if (!physical->GetData(chunk)) break;
            if (chunk.size() == 0) continue;
            final_result.chunks.push_back(std::move(chunk));
            chunk = DataChunk{};
        }

        // Re-tag DATE / TIMESTAMP result columns. The engine carries Parquet
        // date/timestamp columns as INTEGER/BIGINT epoch integers through
        // decode, filtering and aggregation; here — past every hot path —
        // the matching result columns are re-tagged so QueryResult::GetValue
        // boxes a DATE/TIMESTAMP Value and the output renders an ISO string
        // (matching standard SQL / DuckDB) instead of the raw integer. Only
        // the logical type changes; the INT32/INT64 buffers are untouched
        // and layout-compatible (DATE==INT32, TIMESTAMP==INT64).
        // `result_display_types` was captured right after binding because
        // Planner::Plan moves the bound select-list out of the statement.
        for (idx_t c = 0;
             c < result_display_types.size() &&
             c < final_result.column_types.size();
             c++) {
            auto did = result_display_types[c].id();
            if (did != LogicalTypeId::DATE && did != LogicalTypeId::TIMESTAMP)
                continue;
            // Guard: only re-tag when the produced column's physical layout
            // matches (DATE<-INT32, TIMESTAMP<-INT64). A defensive check — a
            // mismatch would mean the plan emitted an unexpected type, so
            // leave such a column alone.
            auto cur = final_result.column_types[c].id();
            bool ok = (did == LogicalTypeId::DATE &&
                       cur == LogicalTypeId::INTEGER) ||
                      (did == LogicalTypeId::TIMESTAMP &&
                       cur == LogicalTypeId::BIGINT);
            if (!ok) continue;
            final_result.column_types[c] = result_display_types[c];
            for (auto &ch : final_result.chunks) {
                if (c < ch.ColumnCount())
                    ch.GetVector(c).SetType(result_display_types[c]);
            }
        }

        // Handle set operations (UNION / UNION ALL / INTERSECT / EXCEPT).
        //
        // Walks the right-leaning chain produced by the parser
        //   `a UNION b UNION c` → sel(a, set_op=UNION, set_right=sel(b,
        //                            set_op=UNION, set_right=sel(c)))
        // so 3+ branches now combine correctly (previously only the
        // first set_right ran, dropping all deeper branches).
        //
        // Stashes the leaf branch's ORDER BY / LIMIT / OFFSET before the
        // walk because the parser puts them there but SQL standard says
        // they apply to the union as a whole, then re-applies them to
        // the combined rows via ApplyUnionOrderLimit.
        //
        // Uses the same SetOpRowSet dedup as derived-table materialisation
        // (NULL-aware, type-aware — INT 1 no longer collides with
        // VARCHAR '1' the way the old string-key dedup did).
        if (stmt->GetType() == StatementType::SELECT) {
            auto &sel = static_cast<SelectStatement &>(*stmt);
            if (!sel.set_op.empty() && sel.set_right) {
                final_result.MaterialiseRows();
                final_result.chunks.clear();
                final_result.chunk_index_built = false;

                SelectStatement *leaf = sel.set_right.get();
                while (leaf->set_right) leaf = leaf->set_right.get();
                std::vector<OrderByItem> stashed_order_by =
                    std::move(leaf->order_by);
                ParsedExprPtr stashed_limit  = std::move(leaf->limit);
                ParsedExprPtr stashed_offset = std::move(leaf->offset);

                // Pre-bind every RIGHT branch so we know per-column types
                // before any execution and any dedup work. Without this,
                // INTERSECT INT vs DOUBLE returned 0 rows (the two Values
                // hashed differently because their types differed even
                // when values were equal). Mirror of the derived-table
                // path at lines ~2236-2299.
                std::vector<std::unique_ptr<BoundStatement>> right_bounds;
                {
                    SelectStatement *cur = &sel;
                    while (!cur->set_op.empty() && cur->set_right) {
                        Binder rb(db_.GetCatalog());
                        right_bounds.push_back(rb.Bind(*cur->set_right));
                        cur = cur->set_right.get();
                    }
                }

                // Per-column common type: start from LEFT branch's types
                // (already materialised in final_result.column_types) and
                // pairwise reduce against each right branch. Cap loop at
                // min(left.cols, right.cols).
                auto result_types = final_result.column_types;
                for (auto &rb_stmt : right_bounds) {
                    auto &rb = static_cast<BoundSelectStatement &>(*rb_stmt);
                    for (idx_t c = 0;
                         c < result_types.size() && c < rb.result_types.size();
                         c++) {
                        result_types[c] = ComputeSetOpCommonType(
                            result_types[c], rb.result_types[c]);
                    }
                }

                // Coerce already-materialised LEFT rows to the new common
                // types (only when the type changed).
                std::vector<bool> col_changed(result_types.size(), false);
                for (idx_t c = 0; c < result_types.size() &&
                     c < final_result.column_types.size(); c++) {
                    col_changed[c] = (result_types[c].id() !=
                                      final_result.column_types[c].id());
                }
                for (auto &row : final_result.rows) {
                    for (idx_t c = 0;
                         c < result_types.size() && c < row.size(); c++) {
                        if (col_changed[c]) {
                            row[c] = CoerceValueForSetOp(row[c], result_types[c]);
                        }
                    }
                }

                SelectStatement *cur = &sel;
                size_t branch_idx = 0;
                while (!cur->set_op.empty() && cur->set_right) {
                    auto right_logical = Planner::Plan(*right_bounds[branch_idx++]);
                    PhysicalPlanner right_pp(db_.GetCatalog());
                    auto right_physical = right_pp.Plan(*right_logical);
                    right_physical->Init();

                    std::vector<std::vector<Value>> right_rows;
                    DataChunk right_chunk;
                    while (true) {
                        if (!right_physical->GetData(right_chunk)) break;
                        for (idx_t ri = 0; ri < right_chunk.size(); ri++) {
                            std::vector<Value> row;
                            row.reserve(right_chunk.ColumnCount());
                            for (idx_t c = 0; c < right_chunk.ColumnCount(); c++) {
                                row.push_back(CoerceValueForSetOp(
                                    right_chunk.GetValue(c, ri),
                                    c < result_types.size()
                                        ? result_types[c]
                                        : right_chunk.GetVector(c).GetType()));
                            }
                            right_rows.push_back(std::move(row));
                        }
                    }

                    const std::string &op = cur->set_op;
                    if (op == "UNION ALL") {
                        for (auto &row : right_rows) {
                            final_result.rows.push_back(std::move(row));
                        }
                    } else if (op == "UNION") {
                        SetOpRowSet seen;
                        for (auto &row : final_result.rows) seen.insert(row);
                        for (auto &row : right_rows) {
                            if (seen.insert(row).second)
                                final_result.rows.push_back(std::move(row));
                        }
                    } else if (op == "INTERSECT") {
                        SetOpRowSet rk(right_rows.begin(), right_rows.end());
                        std::vector<std::vector<Value>> kept;
                        for (auto &row : final_result.rows) {
                            if (rk.count(row)) kept.push_back(std::move(row));
                        }
                        final_result.rows = std::move(kept);
                    } else if (op == "EXCEPT") {
                        SetOpRowSet rk(right_rows.begin(), right_rows.end());
                        std::vector<std::vector<Value>> kept;
                        for (auto &row : final_result.rows) {
                            if (!rk.count(row)) kept.push_back(std::move(row));
                        }
                        final_result.rows = std::move(kept);
                    }
                    cur = cur->set_right.get();
                }

                // Update the result schema to the unified types. Names
                // stay as the LEFT branch's per SQL standard.
                final_result.column_types = result_types;

                if (!stashed_order_by.empty() || stashed_limit ||
                    stashed_offset) {
                    ApplyUnionOrderLimit(final_result.rows,
                                         stashed_order_by,
                                         stashed_limit, stashed_offset,
                                         final_result.column_names);
                }
            }
        }

        // temp_tables / cte_tables / _cte_temp_tables are dropped by the
        // TableCleanupGuard declared at the top of this loop iteration -
        // runs on both success and exception paths.
    }

    return final_result;
}

// ============================================================================
// Programmatic API (unchanged from before)
// ============================================================================

void Connection::CreateTable(const std::string &name,
                              std::vector<ColumnDefinition> columns) {
    auto types = std::vector<LogicalType>();
    types.reserve(columns.size());
    for (auto &col : columns) {
        types.push_back(col.type);
    }

    auto &entry = db_.GetCatalog().CreateTable(name, std::move(columns));
    entry.SetStorage(std::make_shared<DataTable>(types));
}

void Connection::DropTable(const std::string &name) {
    if (!db_.GetCatalog().DropTable(name)) {
        throw CatalogException("Table '" + name + "' does not exist");
    }
}

void Connection::Append(const std::string &table_name, DataChunk &chunk) {
    auto *entry = db_.GetCatalog().GetTable(table_name);
    if (!entry) {
        throw CatalogException("Table '" + table_name + "' not found");
    }
    entry->GetStorage().Append(chunk);
}

void Connection::Scan(const std::string &table_name,
                       std::function<void(DataChunk &)> callback) {
    auto *entry = db_.GetCatalog().GetTable(table_name);
    if (!entry) {
        throw CatalogException("Table '" + table_name + "' not found");
    }

    auto &table = entry->GetStorage();
    auto state = table.InitScan();
    DataChunk result;
    result.Initialize(table.GetTypes());

    while (table.Scan(state, result)) {
        callback(result);
    }
}

// ============================================================================
// Transaction control
// ============================================================================

void Connection::BeginTransaction() {
    if (active_txn_) {
        throw TransactionException("Transaction already active");
    }
    active_txn_ = &db_.GetTransactionManager().BeginTransaction();
}

void Connection::CommitTransaction() {
    if (!active_txn_) {
        throw TransactionException("No active transaction to commit");
    }
    db_.GetTransactionManager().CommitTransaction(*active_txn_);
    active_txn_ = nullptr;
}

void Connection::RollbackTransaction() {
    if (!active_txn_) {
        throw TransactionException("No active transaction to rollback");
    }
    db_.GetTransactionManager().RollbackTransaction(*active_txn_);
    active_txn_ = nullptr;
}

// ============================================================================
// QueryResult
// ============================================================================

std::string QueryResult::ToString() const {
    std::ostringstream ss;
    for (idx_t i = 0; i < ColumnCount(); i++) {
        if (i > 0) ss << "\t";
        ss << column_names[i];
    }
    ss << "\n";
    // If data lives in chunks (SELECT fast path), iterate them directly
    // to avoid the row materialisation cost. Both code paths produce the
    // same per-cell ToString output.
    if (rows.empty() && !chunks.empty()) {
        idx_t ncols = ColumnCount();
        for (auto &c : chunks) {
            idx_t n = c.size();
            for (idx_t r = 0; r < n; r++) {
                for (idx_t k = 0; k < ncols; k++) {
                    if (k > 0) ss << "\t";
                    ss << c.GetValue(k, r).ToString();
                }
                ss << "\n";
            }
        }
    } else {
        for (auto &row : rows) {
            for (idx_t i = 0; i < row.size(); i++) {
                if (i > 0) ss << "\t";
                ss << row[i].ToString();
            }
            ss << "\n";
        }
    }
    return ss.str();
}

} // namespace slothdb

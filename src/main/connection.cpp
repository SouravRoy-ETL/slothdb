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
#include "slothdb/storage/arrow_ipc.hpp"
#include "slothdb/storage/avro_reader.hpp"
#include "slothdb/storage/excel_reader.hpp"
#include "slothdb/storage/http_client.hpp"
#include "slothdb/storage/sqlite_scanner.hpp"
#include "slothdb/storage/hive_partition.hpp"
#include "slothdb/common/string_util.hpp"
#include "slothdb/common/file_glob.hpp"
#include <sstream>
#include <unordered_set>

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

            // Execute the view query once to determine column names and types.
            auto view_result = Query(view_sql);

            if (cv.or_replace) db_.GetCatalog().DropTable(cv.view_name);

            std::vector<ColumnDefinition> cols;
            for (idx_t i = 0; i < view_result.column_names.size(); i++) {
                cols.emplace_back(view_result.column_names[i], view_result.column_types[i]);
            }

            auto &entry = db_.GetCatalog().CreateTable(cv.view_name, std::move(cols));
            auto storage = std::make_shared<DataTable>(view_result.column_types);
            entry.SetStorage(storage);
            entry.SetViewQuery(view_sql);
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

                DataChunk chunk;
                while (true) {
                    if (!sel_physical->GetData(chunk)) break;
                    entry->GetStorage().Append(chunk);
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
                    auto file_pattern = static_cast<ConstantExpression &>(*args[0]).value;
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
                if (copy.source_query->from_table) {
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
                    // CSV (default) — fast stream.
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
        if (stmt->GetType() == StatementType::SELECT) {
            auto &sel = static_cast<SelectStatement &>(*stmt);

            // Expand virtual views in the FROM clause.
            auto expand_view = [&](TableRef &ref) {
                if (ref.is_table_function) return;
                auto *entry = db_.GetCatalog().GetTable(ref.table_name);
                if (entry && entry->IsView()) {
                    // Re-execute the view's stored query to get fresh data.
                    auto view_result = Query(entry->GetViewQuery());
                    auto storage = std::make_shared<DataTable>(view_result.column_types);
                    BulkLoadRows(*storage, view_result.column_types, view_result.rows);
                    entry->SetStorage(storage);
                }
            };

            if (sel.from_table && !sel.from_table->is_table_function) {
                expand_view(*sel.from_table);
                if (sel.from_table->right) expand_view(*sel.from_table->right);
            }
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "GENERATE_SERIES") {
                // Evaluate args as constants.
                int64_t start_val = 0, stop_val = 0, step_val = 1;
                if (sel.from_table->function_args.size() >= 2) {
                    auto &s = static_cast<ConstantExpression &>(*sel.from_table->function_args[0]);
                    start_val = std::stoll(s.value);
                    auto &e = static_cast<ConstantExpression &>(*sel.from_table->function_args[1]);
                    stop_val = std::stoll(e.value);
                }
                if (sel.from_table->function_args.size() >= 3) {
                    auto &st = static_cast<ConstantExpression &>(*sel.from_table->function_args[2]);
                    step_val = std::stoll(st.value);
                }

                std::string tbl_name = sel.from_table->alias.empty()
                    ? "__generate_series__" : sel.from_table->alias;
                sel.from_table->table_name = tbl_name;
                sel.from_table->is_table_function = false;

                auto &entry = db_.GetCatalog().CreateTable(tbl_name,
                    {ColumnDefinition("generate_series", LogicalType::BIGINT())});
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

                auto file_pattern = static_cast<ConstantExpression &>(*args[0]).value;
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

                std::vector<ColumnDefinition> cols;
                for (size_t i = 0; i < header.size() && i < types.size(); i++)
                    cols.emplace_back(header[i], types[i]);

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
                    auto file_path = static_cast<ConstantExpression &>(*args[0]).value;

                    JSONReader reader(file_path);
                    reader.DetectSchema();
                    auto rows = reader.ReadAll();
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? "__read_json__" : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);

                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);

                    BulkLoadRows(*storage, col_types, rows);
                    temp_tables.push_back(tbl_name);
                }
            }

            // Handle read_parquet table function — schema only; scan is streamed
            // at execution time via PhysicalParquetScan.
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_PARQUET") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto file_path = static_cast<ConstantExpression &>(*args[0]).value;

                    // Open just to read the footer metadata (cheap).
                    ParquetReader reader(file_path);
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? "__read_parquet__" : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);

                    if (db_.GetCatalog().GetTable(tbl_name)) db_.GetCatalog().DropTable(tbl_name);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    // Mark as parquet file-backed: PhysicalPlanner::PlanGet will
                    // dispatch to PhysicalParquetScan instead of PhysicalTableScan.
                    entry.SetParquetPath(file_path);
                    temp_tables.push_back(tbl_name);
                }
            }

            // Handle read_arrow table function.
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_ARROW") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto fp = static_cast<ConstantExpression &>(*args[0]).value;
                    ArrowIPCReader reader(fp);
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();
                    auto rows = reader.ReadAll();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? "__read_arrow__" : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    BulkLoadRows(*storage, col_types, rows);
                    temp_tables.push_back(tbl_name);
                }
            }

            // Handle read_avro table function.
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_AVRO") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto fp = static_cast<ConstantExpression &>(*args[0]).value;
                    AvroReader reader(fp);
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();
                    auto rows = reader.ReadAll();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? "__read_avro__" : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    BulkLoadRows(*storage, col_types, rows);
                    temp_tables.push_back(tbl_name);
                }
            }

            // Handle read_xlsx table function.
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "READ_XLSX") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto fp = static_cast<ConstantExpression &>(*args[0]).value;
                    ExcelReader reader(fp);
                    auto col_names = reader.GetColumnNames();
                    auto col_types = reader.GetColumnTypes();
                    auto rows = reader.ReadAll();

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? "__read_xlsx__" : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    for (size_t i = 0; i < col_names.size(); i++)
                        cols.emplace_back(col_names[i], col_types[i]);
                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    BulkLoadRows(*storage, col_types, rows);
                    temp_tables.push_back(tbl_name);
                }
            }

            // Handle sqlite_scan table function.
            if (sel.from_table && sel.from_table->is_table_function &&
                StringUtil::Upper(sel.from_table->table_name) == "SQLITE_SCAN") {
                auto &args = sel.from_table->function_args;
                if (args.size() >= 2 &&
                    args[0]->GetExpressionType() == ExpressionType::CONSTANT &&
                    args[1]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto db_path = static_cast<ConstantExpression &>(*args[0]).value;
                    auto table_name = static_cast<ConstantExpression &>(*args[1]).value;

                    SQLiteScanner scanner(db_path);
                    auto col_info = scanner.GetColumns(table_name);
                    auto rows = scanner.ScanTable(table_name);

                    std::string tbl_name = sel.from_table->alias.empty()
                        ? "__sqlite_scan__" : sel.from_table->alias;
                    sel.from_table->table_name = tbl_name;
                    sel.from_table->is_table_function = false;

                    std::vector<ColumnDefinition> cols;
                    std::vector<LogicalType> col_types;
                    for (auto &ci : col_info) {
                        cols.emplace_back(ci.name, ci.type);
                        col_types.push_back(ci.type);
                    }

                    auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                    auto storage = std::make_shared<DataTable>(col_types);
                    entry.SetStorage(storage);
                    BulkLoadRows(*storage, col_types, rows);
                    temp_tables.push_back(tbl_name);
                }
            }

            // Handle auto-detect: SELECT * FROM 'file.ext'
            if (sel.from_table && sel.from_table->is_table_function &&
                sel.from_table->table_name == "__FILE__") {
                auto &args = sel.from_table->function_args;
                if (!args.empty() && args[0]->GetExpressionType() == ExpressionType::CONSTANT) {
                    auto file_path = static_cast<ConstantExpression &>(*args[0]).value;

                    // Detect format from extension.
                    auto ext = file_path.substr(file_path.find_last_of('.') + 1);
                    for (auto &c : ext) c = static_cast<char>(std::tolower(c));

                    if (ext == "csv" || ext == "tsv") {
                        sel.from_table->table_name = "READ_CSV";
                    } else if (ext == "json" || ext == "ndjson" || ext == "jsonl") {
                        sel.from_table->table_name = "READ_JSON";
                    } else if (ext == "parquet") {
                        sel.from_table->table_name = "READ_PARQUET";
                    } else if (ext == "arrow" || ext == "feather" || ext == "ipc") {
                        sel.from_table->table_name = "READ_ARROW";
                    } else if (ext == "avro") {
                        sel.from_table->table_name = "READ_AVRO";
                    } else if (ext == "xlsx" || ext == "xls") {
                        sel.from_table->table_name = "READ_XLSX";
                    } else if (ext == "db" || ext == "sqlite" || ext == "sqlite3") {
                        sel.from_table->table_name = "SQLITE_SCAN";
                    } else {
                        throw IOException("Unknown file format: ." + ext);
                    }
                    // Re-process with the detected table function name.
                    // (The loop will pick it up on the next iteration — but we're
                    //  in the same iteration. Let's handle inline.)
                    if (sel.from_table->table_name == "READ_CSV") {
                        char delim = (ext == "tsv") ? '\t' : ',';
                        FastCSVReader reader(file_path, delim);
                        auto header = reader.ReadHeader();
                        auto types = reader.DetectTypes();

                        std::string tbl_name = sel.from_table->alias.empty()
                            ? "__auto_file__" : sel.from_table->alias;
                        sel.from_table->table_name = tbl_name;
                        sel.from_table->is_table_function = false;

                        std::vector<ColumnDefinition> cols;
                        for (size_t i = 0; i < header.size() && i < types.size(); i++)
                            cols.emplace_back(header[i], types[i]);
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
                            ? "__auto_file__" : sel.from_table->alias;
                        sel.from_table->table_name = tbl_name;
                        sel.from_table->is_table_function = false;

                        std::vector<ColumnDefinition> cols;
                        for (size_t i = 0; i < col_names.size(); i++)
                            cols.emplace_back(col_names[i], col_types[i]);
                        auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                        auto storage = std::make_shared<DataTable>(col_types);
                        entry.SetStorage(storage);
                        BulkLoadRows(*storage, col_types, rows);
                        temp_tables.push_back(tbl_name);
                    } else if (sel.from_table->table_name == "READ_PARQUET") {
                        ParquetReader reader(file_path);
                        auto col_names = reader.GetColumnNames();
                        auto col_types = reader.GetColumnTypes();
                        auto rows = reader.ReadAll();

                        std::string tbl_name = sel.from_table->alias.empty()
                            ? "__auto_file__" : sel.from_table->alias;
                        sel.from_table->table_name = tbl_name;
                        sel.from_table->is_table_function = false;

                        std::vector<ColumnDefinition> cols;
                        for (size_t i = 0; i < col_names.size(); i++)
                            cols.emplace_back(col_names[i], col_types[i]);
                        auto &entry = db_.GetCatalog().CreateTable(tbl_name, std::move(cols));
                        auto storage = std::make_shared<DataTable>(col_types);
                        entry.SetStorage(storage);
                        BulkLoadRows(*storage, col_types, rows);
                        temp_tables.push_back(tbl_name);
                    }
                }
            }
        }

        // Handle CTEs: materialize each CTE as a temporary table.
        std::vector<std::string> cte_tables;
        std::vector<std::string> _cte_temp_tables; // collected by helper below

        // Helper to expand read_csv() in any TableRef.
        auto cte_process_read_csv = [&](TableRef &tref) -> bool {
            if (!tref.is_table_function) return false;
            auto upper = StringUtil::Upper(tref.table_name);
            if (upper != "READ_CSV" && upper != "READ_CSV_AUTO") return false;
            auto &args = tref.function_args;
            if (args.empty() || args[0]->GetExpressionType() != ExpressionType::CONSTANT) return false;

            auto file_pattern = static_cast<ConstantExpression &>(*args[0]).value;
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
                    cte_tables.push_back(cte.name);
                } else {
                    // Non-recursive CTE — pre-process read_csv in the inner SELECT.
                    auto cte_stmt = std::make_unique<SelectStatement>();
                    *cte_stmt = std::move(*cte.query);

                    // Apply read_csv expansion to CTE's from_table (and any join right side).
                    if (cte_stmt->from_table) {
                        cte_process_read_csv(*cte_stmt->from_table);
                        if (cte_stmt->from_table->right) cte_process_read_csv(*cte_stmt->from_table->right);
                    }

                    Binder cte_binder(db_.GetCatalog());
                    auto cte_bound = cte_binder.Bind(*cte_stmt);
                    auto cte_logical = Planner::Plan(*cte_bound);
                    cte_logical = Optimizer::Optimize(std::move(cte_logical));
                    PhysicalPlanner cte_pp(db_.GetCatalog());
                    auto cte_physical = cte_pp.Plan(*cte_logical);
                    cte_physical->Init();

                    auto &cte_sel = static_cast<BoundSelectStatement &>(*cte_bound);
                    std::vector<ColumnDefinition> cte_cols;
                    for (idx_t i = 0; i < cte_sel.result_names.size(); i++)
                        cte_cols.emplace_back(cte_sel.result_names[i], cte_sel.result_types[i]);

                    auto &entry = db_.GetCatalog().CreateTable(cte.name, std::move(cte_cols));
                    auto storage = std::make_shared<DataTable>(cte_sel.result_types);
                    entry.SetStorage(storage);

                    DataChunk cte_chunk;
                    while (true) {
                        if (!cte_physical->GetData(cte_chunk)) break;
                        storage->Append(cte_chunk);
                    }
                    cte_tables.push_back(cte.name);
                }
            }
        }

        // 2. Bind.
        Binder binder(db_.GetCatalog());
        auto bound = binder.Bind(*stmt);

        // 3. Logical plan.
        auto logical = Planner::Plan(*bound);

        // 3b. Optimize logical plan.
        logical = Optimizer::Optimize(std::move(logical));

        // 4. Physical plan.
        PhysicalPlanner physical_planner(db_.GetCatalog());
        auto physical = physical_planner.Plan(*logical);

        // 5. Execute.
        ExpressionExecutor::SetCatalog(&db_.GetCatalog());
        physical->Init();

        // Collect result column names/types from bound statement.
        if (bound->GetType() == BoundStatementType::SELECT) {
            auto &sel = static_cast<BoundSelectStatement &>(*bound);
            final_result.column_names = sel.result_names;
            final_result.column_types = sel.result_types;
        }

        DataChunk chunk;
        while (true) {
            if (!physical->GetData(chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                    row.push_back(chunk.GetValue(col, i));
                }
                final_result.rows.push_back(std::move(row));
            }
        }

        // Handle set operations (UNION, INTERSECT, EXCEPT).
        if (stmt->GetType() == StatementType::SELECT) {
            auto &sel = static_cast<SelectStatement &>(*stmt);
            if (!sel.set_op.empty() && sel.set_right) {
                // Execute the right side.
                Binder right_binder(db_.GetCatalog());
                auto right_bound = right_binder.Bind(*sel.set_right);
                auto right_logical = Planner::Plan(*right_bound);
                PhysicalPlanner right_pp(db_.GetCatalog());
                auto right_physical = right_pp.Plan(*right_logical);
                right_physical->Init();

                std::vector<std::vector<Value>> right_rows;
                DataChunk right_chunk;
                while (true) {
                    if (!right_physical->GetData(right_chunk)) break;
                    for (idx_t ri = 0; ri < right_chunk.size(); ri++) {
                        std::vector<Value> row;
                        for (idx_t c = 0; c < right_chunk.ColumnCount(); c++)
                            row.push_back(right_chunk.GetValue(c, ri));
                        right_rows.push_back(std::move(row));
                    }
                }

                if (sel.set_op == "UNION") {
                    // Add right rows, removing duplicates.
                    std::unordered_set<std::string> seen;
                    for (auto &row : final_result.rows) {
                        std::string key;
                        for (auto &v : row) key += v.ToString() + "|";
                        seen.insert(key);
                    }
                    for (auto &row : right_rows) {
                        std::string key;
                        for (auto &v : row) key += v.ToString() + "|";
                        if (seen.insert(key).second) {
                            final_result.rows.push_back(std::move(row));
                        }
                    }
                } else if (sel.set_op == "UNION ALL") {
                    for (auto &row : right_rows) {
                        final_result.rows.push_back(std::move(row));
                    }
                } else if (sel.set_op == "INTERSECT") {
                    std::unordered_set<std::string> right_keys;
                    for (auto &row : right_rows) {
                        std::string key;
                        for (auto &v : row) key += v.ToString() + "|";
                        right_keys.insert(key);
                    }
                    std::vector<std::vector<Value>> intersected;
                    for (auto &row : final_result.rows) {
                        std::string key;
                        for (auto &v : row) key += v.ToString() + "|";
                        if (right_keys.count(key)) {
                            intersected.push_back(std::move(row));
                        }
                    }
                    final_result.rows = std::move(intersected);
                } else if (sel.set_op == "EXCEPT") {
                    std::unordered_set<std::string> right_keys;
                    for (auto &row : right_rows) {
                        std::string key;
                        for (auto &v : row) key += v.ToString() + "|";
                        right_keys.insert(key);
                    }
                    std::vector<std::vector<Value>> excepted;
                    for (auto &row : final_result.rows) {
                        std::string key;
                        for (auto &v : row) key += v.ToString() + "|";
                        if (!right_keys.count(key)) {
                            excepted.push_back(std::move(row));
                        }
                    }
                    final_result.rows = std::move(excepted);
                }
            }
        }

        // Clean up CTE and temp tables.
        for (auto &cte_name : cte_tables) {
            db_.GetCatalog().DropTable(cte_name);
        }
        for (auto &tmp : temp_tables) {
            db_.GetCatalog().DropTable(tmp);
        }
        for (auto &tmp : _cte_temp_tables) {
            db_.GetCatalog().DropTable(tmp);
        }
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
    // Header.
    for (idx_t i = 0; i < ColumnCount(); i++) {
        if (i > 0) ss << "\t";
        ss << column_names[i];
    }
    ss << "\n";
    // Rows.
    for (auto &row : rows) {
        for (idx_t i = 0; i < row.size(); i++) {
            if (i > 0) ss << "\t";
            ss << row[i].ToString();
        }
        ss << "\n";
    }
    return ss.str();
}

} // namespace slothdb

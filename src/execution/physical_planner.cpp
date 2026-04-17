#include "slothdb/execution/physical_planner.hpp"
#include "slothdb/execution/expression_executor.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include "slothdb/storage/data_table.hpp"
#include "slothdb/storage/fast_csv_reader.hpp"
#include "slothdb/storage/parquet.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

namespace slothdb {

// ============================================================================
// Physical Operators
// ============================================================================

class PhysicalTableScan : public PhysicalOperator {
public:
    PhysicalTableScan(TableCatalogEntry *table)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, table->GetTypes()),
          table_(table) {}

    void Init() override {
        state_ = table_->GetStorage().InitScan();
    }

    bool GetData(DataChunk &result) override {
        result.Initialize(GetTypes());
        return table_->GetStorage().Scan(state_, result);
    }

private:
    TableCatalogEntry *table_;
    TableScanState state_;
};

// Streaming file scan — reads CSV directly from file during execution.
// Never materializes the entire file into memory.
class PhysicalFileScan : public PhysicalOperator {
public:
    PhysicalFileScan(const std::string &file_path, char delimiter,
                     std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path), delimiter_(delimiter) {}

    void Init() override {
        reader_ = std::make_unique<FastCSVReader>(file_path_, delimiter_);
        reader_->ReadHeader();
    }

    bool GetData(DataChunk &result) override {
        // Initialize if chunk is fresh; reuse + reset if already set up.
        if (result.ColumnCount() != GetTypes().size()) {
            result.Initialize(GetTypes());
        } else {
            result.Reset();
        }
        idx_t count;
        if (!projection_.empty()) {
            count = reader_->ReadChunkProjected(result, GetTypes(), projection_);
        } else {
            count = reader_->ReadChunk(result, GetTypes());
        }
        return count > 0;
    }

    // Column pruning: tell the scan which columns are needed.
    void SetProjection(std::vector<bool> mask) { projection_ = std::move(mask); }

private:
public:
    const std::string &GetFilePath() const { return file_path_; }
    char GetDelimiter() const { return delimiter_; }
    FastCSVReader *GetReader() { return reader_.get(); }

private:
    std::string file_path_;
    char delimiter_;
    std::unique_ptr<FastCSVReader> reader_;
    bool initialized_ = false;
    std::vector<bool> projection_;
};

// Streaming Parquet scan — reads row groups during execution.
class PhysicalParquetScan : public PhysicalOperator {
public:
    PhysicalParquetScan(const std::string &file_path, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path) {}

    void Init() override {
        reader_ = std::make_unique<ParquetReader>(file_path_);
        rg_pos_ = 0;
        chunks_in_rg_ = 0;
        current_rg_data_.clear();
    }

    bool GetData(DataChunk &result) override {
        result.Initialize(GetTypes());
        result.Reset();

        // Read next row group when current is exhausted.
        if (current_rg_data_.empty() || chunks_in_rg_ >= current_rg_data_.size()) {
            if (rg_pos_ >= reader_->NumRowGroups()) return false;
            current_rg_data_ = reader_->ReadRowGroup(rg_pos_);
            rg_pos_++;
            chunks_in_rg_ = 0;
            if (current_rg_data_.empty()) return false;
        }

        // Pack up to VECTOR_SIZE rows from current row group into chunk.
        idx_t avail = static_cast<idx_t>(current_rg_data_.size()) - chunks_in_rg_;
        idx_t count = std::min<idx_t>(avail, VECTOR_SIZE);
        idx_t num_cols = static_cast<idx_t>(GetTypes().size());

        for (idx_t r = 0; r < count; r++) {
            auto &row = current_rg_data_[chunks_in_rg_ + r];
            for (idx_t c = 0; c < num_cols && c < row.size(); c++) {
                result.SetValue(c, r, row[c]);
            }
        }
        result.SetCardinality(count);
        chunks_in_rg_ += count;
        return count > 0;
    }

    void SetProjection(std::vector<bool> mask) { projection_ = std::move(mask); }
    const std::string &GetFilePath() const { return file_path_; }

private:
    std::string file_path_;
    std::unique_ptr<ParquetReader> reader_;
    idx_t rg_pos_ = 0;
    idx_t chunks_in_rg_ = 0;
    std::vector<std::vector<Value>> current_rg_data_;
    std::vector<bool> projection_;
};

// Ultra-fast scan that only counts rows — no field parsing at all.
class PhysicalCountScan : public PhysicalOperator {
public:
    PhysicalCountScan(const std::string &file_path, char delimiter,
                      std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          file_path_(file_path), delimiter_(delimiter) {}

    void Init() override {
        reader_ = std::make_unique<FastCSVReader>(file_path_, delimiter_);
        reader_->ReadHeader();
        counted_ = false;
        row_count_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (counted_) return false;
        counted_ = true;
        row_count_ = reader_->CountRows();
        // Return a single-row chunk with the count in a BIGINT column.
        result.Initialize({LogicalType::BIGINT()});
        result.GetVector(0).GetData<int64_t>()[0] = static_cast<int64_t>(row_count_);
        result.SetCardinality(1);
        return true;
    }

    idx_t GetRowCount() const { return row_count_; }

private:
    std::string file_path_;
    char delimiter_;
    std::unique_ptr<FastCSVReader> reader_;
    bool counted_ = false;
    idx_t row_count_ = 0;
};

class PhysicalFilter : public PhysicalOperator {
public:
    PhysicalFilter(BoundExprPtr condition, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::FILTER, std::move(types)),
          condition_(std::move(condition)) {}

    void Init() override {
        for (auto &child : children) child->Init();
    }

    bool GetData(DataChunk &result) override {
        while (true) {
            DataChunk input;
            input.Initialize(GetTypes());
            if (!children[0]->GetData(input)) return false;
            if (input.size() == 0) continue;

            // Evaluate filter condition.
            Vector filter_result(LogicalType::BOOLEAN(), input.size());
            ExpressionExecutor::Execute(*condition_, input, filter_result, input.size());

            // Collect matching rows.
            auto *filter_data = filter_result.GetData<bool>();
            result.Initialize(GetTypes());
            idx_t result_count = 0;

            for (idx_t i = 0; i < input.size(); i++) {
                if (filter_result.GetValidity().RowIsValid(i) && filter_data[i]) {
                    for (idx_t col = 0; col < input.ColumnCount(); col++) {
                        result.SetValue(col, result_count, input.GetValue(col, i));
                    }
                    result_count++;
                }
            }

            if (result_count > 0) {
                result.SetCardinality(result_count);
                return true;
            }
            // All rows filtered out, try next chunk.
        }
    }

private:
    BoundExprPtr condition_;
};

class PhysicalProjection : public PhysicalOperator {
public:
    PhysicalProjection(std::vector<BoundExprPtr> expressions, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::PROJECTION, std::move(types)),
          expressions_(std::move(expressions)) {}

    void Init() override {
        for (auto &child : children) child->Init();
    }

    bool GetData(DataChunk &result) override {
        DataChunk input;
        if (!children.empty()) {
            input.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(input)) return false;
            if (input.size() == 0) return false;
        }

        result.Initialize(GetTypes());
        idx_t count = children.empty() ? 1 : input.size();

        for (idx_t col = 0; col < expressions_.size(); col++) {
            ExpressionExecutor::Execute(*expressions_[col], input,
                                        result.GetVector(col), count);
        }
        result.SetCardinality(count);
        return true;
    }

private:
    std::vector<BoundExprPtr> expressions_;
};

class PhysicalOrderBy : public PhysicalOperator {
public:
    PhysicalOrderBy(std::vector<BoundOrderBy> orders, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::ORDER_BY, std::move(types)),
          orders_(std::move(orders)) {}

    void Init() override {
        for (auto &child : children) child->Init();
        collected_ = false;
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        // Collect all input first.
        if (!collected_) {
            CollectAll();
            collected_ = true;
        }

        if (emit_pos_ >= sorted_rows_.size()) return false;

        result.Initialize(GetTypes());
        idx_t count = 0;
        while (emit_pos_ < sorted_rows_.size() && count < VECTOR_SIZE) {
            auto &row = sorted_rows_[emit_pos_];
            for (idx_t col = 0; col < row.size(); col++) {
                result.SetValue(col, count, row[col]);
            }
            emit_pos_++;
            count++;
        }
        result.SetCardinality(count);
        return count > 0;
    }

private:
    void CollectAll() {
        DataChunk chunk;
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                    row.push_back(chunk.GetValue(col, i));
                }
                sorted_rows_.push_back(std::move(row));
            }
        }

        // Sort using the order expressions.
        // For now, we evaluate order expressions as column indices.
        std::sort(sorted_rows_.begin(), sorted_rows_.end(),
            [this](const std::vector<Value> &a, const std::vector<Value> &b) {
                for (auto &order : orders_) {
                    // Get the column index from the order expression.
                    idx_t col_idx = 0;
                    if (order.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        col_idx = static_cast<BoundColumnRef &>(*order.expression).column_index;
                    }

                    auto &va = a[col_idx];
                    auto &vb = b[col_idx];

                    if (va.IsNull() && vb.IsNull()) continue;
                    if (va.IsNull()) return !order.ascending;
                    if (vb.IsNull()) return order.ascending;

                    if (va < vb) return order.ascending;
                    if (vb < va) return !order.ascending;
                }
                return false;
            });
    }

    std::vector<BoundOrderBy> orders_;
    std::vector<std::vector<Value>> sorted_rows_;
    bool collected_ = false;
    idx_t emit_pos_ = 0;
};

class PhysicalLimit : public PhysicalOperator {
public:
    PhysicalLimit(int64_t limit_count, int64_t offset_count, std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::LIMIT, std::move(types)),
          limit_count_(limit_count), offset_count_(offset_count) {}

    void Init() override {
        for (auto &child : children) child->Init();
        rows_emitted_ = 0;
        rows_skipped_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (limit_count_ >= 0 && rows_emitted_ >= static_cast<idx_t>(limit_count_)) {
            return false;
        }

        while (true) {
            DataChunk input;
            input.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(input)) return false;

            result.Initialize(GetTypes());
            idx_t result_count = 0;

            for (idx_t i = 0; i < input.size(); i++) {
                if (rows_skipped_ < static_cast<idx_t>(offset_count_)) {
                    rows_skipped_++;
                    continue;
                }
                if (limit_count_ >= 0 &&
                    rows_emitted_ >= static_cast<idx_t>(limit_count_)) {
                    break;
                }
                for (idx_t col = 0; col < input.ColumnCount(); col++) {
                    result.SetValue(col, result_count, input.GetValue(col, i));
                }
                result_count++;
                rows_emitted_++;
            }

            if (result_count > 0) {
                result.SetCardinality(result_count);
                return true;
            }
        }
    }

private:
    int64_t limit_count_;
    int64_t offset_count_;
    idx_t rows_emitted_ = 0;
    idx_t rows_skipped_ = 0;
};

class PhysicalInsert : public PhysicalOperator {
public:
    PhysicalInsert(TableCatalogEntry *table, std::vector<std::vector<BoundExprPtr>> values)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), values_(std::move(values)) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        DataChunk chunk;
        chunk.Initialize(types);

        for (auto &row : values_) {
            idx_t row_idx = chunk.size();
            for (idx_t col = 0; col < row.size(); col++) {
                auto val = ExpressionExecutor::ExecuteScalar(*row[col]);
                chunk.SetValue(col, row_idx, val);
            }
        }

        table_->GetStorage().Append(chunk);
        return false; // No result set.
    }

private:
    TableCatalogEntry *table_;
    std::vector<std::vector<BoundExprPtr>> values_;
    bool done_ = false;
};

class PhysicalCreateTable : public PhysicalOperator {
public:
    PhysicalCreateTable(Catalog &catalog, const std::string &name,
                        std::vector<ColumnDefinition> columns, bool if_not_exists)
        : PhysicalOperator(PhysicalOperatorType::CREATE_TABLE, {}),
          catalog_(catalog), table_name_(name), columns_(std::move(columns)),
          if_not_exists_(if_not_exists) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        if (if_not_exists_ && catalog_.GetTable(table_name_)) {
            return false;
        }

        auto types_vec = std::vector<LogicalType>();
        for (auto &col : columns_) types_vec.push_back(col.type);

        auto &entry = catalog_.CreateTable(table_name_, columns_);
        entry.SetStorage(std::make_shared<DataTable>(types_vec));
        return false;
    }

private:
    Catalog &catalog_;
    std::string table_name_;
    std::vector<ColumnDefinition> columns_;
    bool if_not_exists_;
    bool done_ = false;
};

class PhysicalDropTable : public PhysicalOperator {
public:
    PhysicalDropTable(Catalog &catalog, const std::string &name, bool if_exists)
        : PhysicalOperator(PhysicalOperatorType::DROP_TABLE, {}),
          catalog_(catalog), table_name_(name), if_exists_(if_exists) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        if (!catalog_.DropTable(table_name_) && !if_exists_) {
            throw CatalogException("Table '" + table_name_ + "' does not exist");
        }
        return false;
    }

private:
    Catalog &catalog_;
    std::string table_name_;
    bool if_exists_;
    bool done_ = false;
};

// ============================================================================
// Window
// ============================================================================

class PhysicalWindow : public PhysicalOperator {
public:
    PhysicalWindow(std::vector<BoundExprPtr> select_list, BoundExprPtr qualify,
                   std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)),
          select_list_(std::move(select_list)), qualify_(std::move(qualify)) {}

    void Init() override {
        for (auto &child : children) child->Init();
        computed_ = false;
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!computed_) {
            ComputeWindows();
            computed_ = true;
        }

        if (emit_pos_ >= result_rows_.size()) return false;

        result.Initialize(GetTypes());
        idx_t count = 0;
        while (emit_pos_ < result_rows_.size() && count < VECTOR_SIZE) {
            auto &row = result_rows_[emit_pos_];
            for (idx_t col = 0; col < row.size(); col++) {
                result.SetValue(col, count, row[col]);
            }
            emit_pos_++;
            count++;
        }
        result.SetCardinality(count);
        return count > 0;
    }

private:
    void ComputeWindows() {
        // 1. Collect all input rows.
        std::vector<std::vector<Value>> all_rows;
        DataChunk chunk;
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
                    row.push_back(chunk.GetValue(col, i));
                }
                all_rows.push_back(std::move(row));
            }
        }

        if (all_rows.empty()) return;

        // 2. For each output column, compute values.
        idx_t num_input_rows = all_rows.size();
        idx_t num_output_cols = select_list_.size();
        // Result: num_input_rows x num_output_cols
        std::vector<std::vector<Value>> output(num_input_rows);
        for (auto &row : output) row.resize(num_output_cols);

        for (idx_t col = 0; col < num_output_cols; col++) {
            auto &expr = select_list_[col];
            if (expr->GetExpressionType() == BoundExpressionType::WINDOW) {
                auto &win = static_cast<BoundWindowExpression &>(*expr);
                ComputeWindowColumn(win, all_rows, output, col);
            } else if (expr->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                auto &ref = static_cast<BoundColumnRef &>(*expr);
                for (idx_t i = 0; i < num_input_rows; i++) {
                    output[i][col] = all_rows[i][ref.column_index];
                }
            } else if (expr->GetExpressionType() == BoundExpressionType::CONSTANT) {
                auto &c = static_cast<BoundConstant &>(*expr);
                for (idx_t i = 0; i < num_input_rows; i++) {
                    output[i][col] = c.value;
                }
            } else {
                // For other expressions, evaluate row-by-row.
                for (idx_t i = 0; i < num_input_rows; i++) {
                    DataChunk single;
                    single.Initialize(children[0]->GetTypes());
                    for (idx_t c = 0; c < all_rows[i].size(); c++) {
                        single.SetValue(c, 0, all_rows[i][c]);
                    }
                    Vector res(expr->GetReturnType());
                    ExpressionExecutor::Execute(*expr, single, res, 1);
                    output[i][col] = res.GetValue(0);
                }
            }
        }

        // 3. Apply QUALIFY filter.
        if (qualify_) {
            // Compute any window functions in the QUALIFY expression
            // as extra columns, then evaluate the comparison.
            // Strategy: add window columns from QUALIFY to the output,
            // evaluate QUALIFY as a comparison on those columns.
            idx_t qual_col = num_output_cols; // index of extra column
            bool added_qual_window = false;

            if (qualify_->GetExpressionType() == BoundExpressionType::COMPARISON) {
                auto &cmp = static_cast<BoundComparison &>(*qualify_);
                // Check if left or right side is a window expression.
                BoundWindowExpression *qual_win = nullptr;
                BoundConstant *qual_const = nullptr;

                if (cmp.left->GetExpressionType() == BoundExpressionType::WINDOW) {
                    qual_win = static_cast<BoundWindowExpression *>(cmp.left.get());
                    if (cmp.right->GetExpressionType() == BoundExpressionType::CONSTANT)
                        qual_const = static_cast<BoundConstant *>(cmp.right.get());
                } else if (cmp.right->GetExpressionType() == BoundExpressionType::WINDOW) {
                    qual_win = static_cast<BoundWindowExpression *>(cmp.right.get());
                    if (cmp.left->GetExpressionType() == BoundExpressionType::CONSTANT)
                        qual_const = static_cast<BoundConstant *>(cmp.left.get());
                }

                if (qual_win) {
                    // Extend output with one more column for the QUALIFY window.
                    for (auto &row : output) row.push_back(Value());
                    ComputeWindowColumn(*qual_win, all_rows, output, qual_col);
                    added_qual_window = true;

                    // Now filter based on the comparison.
                    for (idx_t i = 0; i < num_input_rows; i++) {
                        auto &win_val = output[i][qual_col];
                        bool pass = false;
                        if (qual_const && !win_val.IsNull()) {
                            auto &const_val = qual_const->value;
                            if (cmp.op == "=") pass = (win_val.ToString() == const_val.ToString());
                            else if (cmp.op == "<") pass = win_val < const_val;
                            else if (cmp.op == "<=") pass = win_val <= const_val;
                            else if (cmp.op == ">") pass = win_val > const_val;
                            else if (cmp.op == ">=") pass = win_val >= const_val;
                            else if (cmp.op == "!=" || cmp.op == "<>") pass = win_val != const_val;
                        }
                        if (pass) {
                            // Remove the extra qualify column before adding to results.
                            auto row = output[i];
                            row.resize(num_output_cols);
                            result_rows_.push_back(std::move(row));
                        }
                    }
                    return; // Done.
                }
            }
        }

        // No QUALIFY or simple QUALIFY — add all rows.
        for (idx_t i = 0; i < num_input_rows; i++) {
            result_rows_.push_back(output[i]);
        }
    }

    void ComputeWindowColumn(BoundWindowExpression &win,
                              const std::vector<std::vector<Value>> &all_rows,
                              std::vector<std::vector<Value>> &output,
                              idx_t out_col) {
        idx_t n = all_rows.size();
        auto &name = win.function_name;

        // Build partition groups: partition_key -> list of row indices.
        std::unordered_map<std::string, std::vector<idx_t>> partitions;
        std::vector<std::string> row_partition_keys(n);
        std::vector<std::string> partition_order;

        for (idx_t i = 0; i < n; i++) {
            std::string key;
            for (auto &p : win.partition_by) {
                if (p->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                    auto &ref = static_cast<BoundColumnRef &>(*p);
                    key += all_rows[i][ref.column_index].ToString() + "|";
                }
            }
            row_partition_keys[i] = key;
            if (partitions.find(key) == partitions.end()) {
                partition_order.push_back(key);
            }
            partitions[key].push_back(i);
        }

        // Sort within each partition by ORDER BY.
        for (auto &key : partition_order) {
            auto &indices = partitions[key];
            if (!win.order_by.empty()) {
                std::sort(indices.begin(), indices.end(),
                    [&](idx_t a, idx_t b) {
                        for (auto &ord : win.order_by) {
                            if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                                auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                                auto &va = all_rows[a][ref.column_index];
                                auto &vb = all_rows[b][ref.column_index];
                                if (va < vb) return ord.ascending;
                                if (vb < va) return !ord.ascending;
                            }
                        }
                        return false;
                    });
            }
        }

        // Compute window function values.
        for (auto &key : partition_order) {
            auto &indices = partitions[key];
            idx_t partition_size = indices.size();

            for (idx_t pos = 0; pos < partition_size; pos++) {
                idx_t row_idx = indices[pos];
                Value result;

                if (name == "ROW_NUMBER") {
                    result = Value::BIGINT(static_cast<int64_t>(pos + 1));
                } else if (name == "RANK") {
                    // Rank: same value = same rank, gaps after ties.
                    int64_t rank = 1;
                    if (pos > 0 && !win.order_by.empty()) {
                        auto &ord = win.order_by[0];
                        if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                            auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                            auto &cur = all_rows[row_idx][ref.column_index];
                            // Count how many rows before this have different value.
                            rank = static_cast<int64_t>(pos + 1);
                            for (idx_t j = 0; j < pos; j++) {
                                auto &prev = all_rows[indices[j]][ref.column_index];
                                if (cur == prev) {
                                    // Same value as some earlier row — use its rank.
                                    rank = output[indices[j]][out_col].GetValue<int64_t>();
                                    break;
                                }
                            }
                        }
                    }
                    result = Value::BIGINT(rank);
                } else if (name == "DENSE_RANK") {
                    int64_t rank = 1;
                    if (pos > 0 && !win.order_by.empty()) {
                        auto &ord = win.order_by[0];
                        if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                            auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                            auto &cur = all_rows[row_idx][ref.column_index];
                            auto &prev = all_rows[indices[pos - 1]][ref.column_index];
                            int64_t prev_rank = output[indices[pos - 1]][out_col].GetValue<int64_t>();
                            rank = (cur == prev) ? prev_rank : prev_rank + 1;
                        }
                    }
                    result = Value::BIGINT(rank);
                } else if (name == "NTILE") {
                    int64_t num_buckets = 1;
                    if (!win.arguments.empty()) {
                        if (win.arguments[0]->GetExpressionType() == BoundExpressionType::CONSTANT) {
                            auto &c = static_cast<BoundConstant &>(*win.arguments[0]);
                            if (c.value.type().id() == LogicalTypeId::INTEGER)
                                num_buckets = c.value.GetValue<int32_t>();
                            else
                                num_buckets = c.value.GetValue<int64_t>();
                        }
                    }
                    int64_t bucket = static_cast<int64_t>(pos * num_buckets / partition_size) + 1;
                    result = Value::BIGINT(bucket);
                } else if (name == "LAG" || name == "LEAD") {
                    int64_t offset_val = 1;
                    if (win.arguments.size() > 1 &&
                        win.arguments[1]->GetExpressionType() == BoundExpressionType::CONSTANT) {
                        auto &c = static_cast<BoundConstant &>(*win.arguments[1]);
                        if (c.value.type().id() == LogicalTypeId::INTEGER)
                            offset_val = c.value.GetValue<int32_t>();
                    }
                    int64_t target_pos = (name == "LAG")
                        ? static_cast<int64_t>(pos) - offset_val
                        : static_cast<int64_t>(pos) + offset_val;

                    if (target_pos >= 0 && target_pos < static_cast<int64_t>(partition_size)) {
                        idx_t target_row = indices[static_cast<idx_t>(target_pos)];
                        if (!win.arguments.empty() &&
                            win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                            auto &ref = static_cast<BoundColumnRef &>(*win.arguments[0]);
                            result = all_rows[target_row][ref.column_index];
                        }
                    } else {
                        result = Value(); // NULL
                    }
                } else if (name == "FIRST_VALUE") {
                    if (!win.arguments.empty() &&
                        win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*win.arguments[0]);
                        result = all_rows[indices[0]][ref.column_index];
                    }
                } else if (name == "LAST_VALUE") {
                    if (!win.arguments.empty() &&
                        win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*win.arguments[0]);
                        result = all_rows[indices[partition_size - 1]][ref.column_index];
                    }
                } else if (name == "SUM" || name == "COUNT" || name == "AVG" ||
                           name == "MIN" || name == "MAX") {
                    // Running aggregate over the whole partition.
                    double sum = 0;
                    int64_t count = 0;
                    Value min_v, max_v;
                    bool has_min = false;
                    for (auto idx : indices) {
                        if (!win.arguments.empty() &&
                            win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                            auto &ref = static_cast<BoundColumnRef &>(*win.arguments[0]);
                            auto &v = all_rows[idx][ref.column_index];
                            if (!v.IsNull()) {
                                count++;
                                auto tid = v.type().id();
                                double d = 0;
                                if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                                else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                                else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                                sum += d;
                                if (!has_min || v < min_v) min_v = v;
                                if (!has_min || v > max_v) max_v = v;
                                has_min = true;
                            }
                        } else {
                            count++;
                        }
                    }
                    if (name == "COUNT") result = Value::BIGINT(count);
                    else if (name == "SUM") result = Value::BIGINT(static_cast<int64_t>(sum));
                    else if (name == "AVG") result = count > 0 ? Value::DOUBLE(sum / count) : Value();
                    else if (name == "MIN") result = has_min ? min_v : Value();
                    else if (name == "MAX") result = has_min ? max_v : Value();
                } else {
                    result = Value(); // Unknown window function.
                }

                output[row_idx][out_col] = result;
            }
        }
    }

    std::vector<BoundExprPtr> select_list_;
    BoundExprPtr qualify_;
    std::vector<std::vector<Value>> result_rows_;
    bool computed_ = false;
    idx_t emit_pos_ = 0;
};

// ============================================================================
// Distinct
// ============================================================================

class PhysicalDistinct : public PhysicalOperator {
public:
    explicit PhysicalDistinct(std::vector<LogicalType> types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(types)) {}

    void Init() override {
        for (auto &child : children) child->Init();
        seen_.clear();
    }

    bool GetData(DataChunk &result) override {
        while (true) {
            DataChunk input;
            input.Initialize(GetTypes());
            if (!children[0]->GetData(input)) return false;

            result.Initialize(GetTypes());
            idx_t result_count = 0;

            for (idx_t i = 0; i < input.size(); i++) {
                // Build a key from all column values.
                std::string key;
                for (idx_t col = 0; col < input.ColumnCount(); col++) {
                    key += input.GetValue(col, i).ToString() + "|";
                }
                if (seen_.insert(key).second) {
                    for (idx_t col = 0; col < input.ColumnCount(); col++) {
                        result.SetValue(col, result_count, input.GetValue(col, i));
                    }
                    result_count++;
                }
            }

            if (result_count > 0) {
                result.SetCardinality(result_count);
                return true;
            }
        }
    }

private:
    std::unordered_set<std::string> seen_;
};

// ============================================================================
// Hash Aggregate
// ============================================================================

class PhysicalHashAggregate : public PhysicalOperator {
public:
    PhysicalHashAggregate(std::vector<BoundExprPtr> groups,
                          std::vector<BoundExprPtr> aggregates,
                          std::vector<LogicalType> result_types)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(result_types)),
          groups_(std::move(groups)), aggregates_(std::move(aggregates)) {}

    void Init() override {
        for (auto &child : children) child->Init();
        computed_ = false;
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!computed_) {
            ComputeAggregates();
            computed_ = true;
        }

        if (emit_pos_ >= result_rows_.size()) return false;

        result.Initialize(GetTypes());
        idx_t count = 0;
        while (emit_pos_ < result_rows_.size() && count < VECTOR_SIZE) {
            auto &row = result_rows_[emit_pos_];
            for (idx_t col = 0; col < row.size(); col++) {
                result.SetValue(col, count, row[col]);
            }
            emit_pos_++;
            count++;
        }
        result.SetCardinality(count);
        return count > 0;
    }

private:
    struct AggState {
        int64_t count = 0;
        double sum = 0;
        double sum_sq = 0;          // for STDDEV/VARIANCE
        bool has_min = false;
        Value min_val;
        double sum_min = 0;         // numeric min for fast comparison
        bool has_max = false;
        Value max_val;
        double sum_max = 0;         // numeric max for fast comparison
        std::vector<double> values; // for MEDIAN
        std::string str_agg;        // for STRING_AGG
        bool str_started = false;
        std::string str_delim;
        bool bool_and = true;       // for BOOL_AND
        bool bool_or = false;       // for BOOL_OR
        std::unordered_set<std::string> distinct_set; // for COUNT(DISTINCT)
    };

    // Fast path: read a double from a vector at index without Value allocation.
    static double ReadDouble(const Vector &vec, idx_t row) {
        auto tid = vec.GetType().id();
        auto *validity = &vec.GetValidity();
        if (!validity->RowIsValid(row)) return 0.0;
        switch (tid) {
        case LogicalTypeId::INTEGER: return static_cast<double>(reinterpret_cast<const int32_t *>(vec.GetData())[row]);
        case LogicalTypeId::BIGINT:  return static_cast<double>(reinterpret_cast<const int64_t *>(vec.GetData())[row]);
        case LogicalTypeId::DOUBLE:  return reinterpret_cast<const double *>(vec.GetData())[row];
        case LogicalTypeId::FLOAT:   return static_cast<double>(reinterpret_cast<const float *>(vec.GetData())[row]);
        default: return 0.0;
        }
    }

    // Fast path: build a group key string from vector data directly.
    static void AppendGroupKey(std::string &key, const Vector &vec, idx_t row) {
        auto tid = vec.GetType().id();
        if (!vec.GetValidity().RowIsValid(row)) { key += "NULL|"; return; }
        switch (tid) {
        case LogicalTypeId::INTEGER: key += std::to_string(reinterpret_cast<const int32_t *>(vec.GetData())[row]); break;
        case LogicalTypeId::BIGINT:  key += std::to_string(reinterpret_cast<const int64_t *>(vec.GetData())[row]); break;
        case LogicalTypeId::DOUBLE:  key += std::to_string(reinterpret_cast<const double *>(vec.GetData())[row]); break;
        case LogicalTypeId::VARCHAR: {
            auto &s = reinterpret_cast<const string_t *>(vec.GetData())[row];
            key.append(s.GetData(), s.GetSize());
            break;
        }
        default: key += vec.GetValue(row).ToString(); break;
        }
        key += '|';
    }

    void ComputeAggregates() {
        // Process chunks directly — no intermediate row materialization.
        std::unordered_map<std::string, std::vector<AggState>> group_states;
        std::unordered_map<std::string, std::vector<Value>> group_keys;
        std::vector<std::string> group_order;

        idx_t num_aggs = aggregates_.size();

        // Pre-resolve aggregate argument column indices for fast path.
        struct AggInfo {
            std::string name;
            idx_t col_idx;        // column index if simple column ref, else INVALID_INDEX
            bool is_count_star;
            bool is_distinct;
        };
        std::vector<AggInfo> agg_infos(num_aggs);
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
            agg_infos[a].name = StringUtil::Upper(agg_expr.function_name);
            agg_infos[a].is_count_star = agg_expr.arguments.empty();
            agg_infos[a].is_distinct = agg_expr.is_distinct;
            agg_infos[a].col_idx = INVALID_INDEX;
            if (!agg_expr.arguments.empty() &&
                agg_expr.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                agg_infos[a].col_idx = static_cast<BoundColumnRef &>(*agg_expr.arguments[0]).column_index;
            }
        }

        // Pre-resolve group-by column indices.
        std::vector<idx_t> group_col_indices;
        for (auto &g : groups_) {
            if (g->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                group_col_indices.push_back(static_cast<BoundColumnRef &>(*g).column_index);
            }
        }

        DataChunk chunk;
        chunk.Initialize(children[0]->GetTypes());
        std::string key;
        idx_t total_rows_processed = 0;

        // === FAST PATH: COUNT(*) with no GROUP BY — just count rows ===
        bool is_simple_count_star = (group_col_indices.empty() && num_aggs == 1 &&
                                     agg_infos[0].is_count_star && agg_infos[0].name == "COUNT");
        // FAST PATH: all COUNT/SUM/AVG on simple columns, no GROUP BY
        bool is_simple_no_group = group_col_indices.empty();
        bool all_simple_aggs = is_simple_no_group;
        for (idx_t a = 0; a < num_aggs && all_simple_aggs; a++) {
            auto &info = agg_infos[a];
            if (info.name == "COUNT" && info.is_count_star) continue;
            if ((info.name == "COUNT" || info.name == "SUM" || info.name == "AVG" ||
                 info.name == "MIN" || info.name == "MAX") && info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
            all_simple_aggs = false;
        }

        // === FAST PATH: single-column GROUP BY with simple aggregates ===
        bool single_group_fast = (group_col_indices.size() == 1 && all_simple_aggs == false &&
                                  num_aggs > 0);
        if (single_group_fast) {
            // Re-check: all aggs must be simple COUNT/SUM/AVG/MIN/MAX on direct columns
            single_group_fast = true;
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &info = agg_infos[a];
                if (info.is_count_star) continue;
                if ((info.name == "COUNT" || info.name == "SUM" || info.name == "AVG" ||
                     info.name == "MIN" || info.name == "MAX") &&
                    info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
                single_group_fast = false;
                break;
            }
        }

        // Parallel single-column GROUP BY (disabled — memory bandwidth limited).
        PhysicalFileScan *file_scan_for_group = nullptr;
        (void)file_scan_for_group;
        if (false) {
            // Initialize file scan to load buffer.
            file_scan_for_group->Init();
            auto *reader = file_scan_for_group->GetReader();
            const char *buffer = reader->GetBuffer();
            size_t total_size = reader->GetSize();
            size_t data_start = reader->GetPos();
            char delim = file_scan_for_group->GetDelimiter();
            size_t data_size = total_size - data_start;

            unsigned int num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0 || num_threads > 8) num_threads = 4;
            if (data_size < 16 * 1024 * 1024) num_threads = 1;

            // Compute per-thread byte ranges aligned to line boundaries.
            std::vector<size_t> ranges(num_threads + 1);
            ranges[0] = data_start;
            ranges[num_threads] = total_size;
            for (unsigned int t = 1; t < num_threads; t++) {
                size_t target = data_start + (data_size * t) / num_threads;
                ranges[t] = FastCSVReader::FindLineStart(buffer, total_size, target);
            }

            idx_t group_col = group_col_indices[0];
            auto types = children[0]->GetTypes();

            struct ThreadState {
                std::unordered_map<int64_t, std::vector<AggState>> int_groups;
                std::unordered_map<std::string, std::vector<AggState>> str_groups;
                std::unordered_map<int64_t, Value> int_keys;
                std::unordered_map<std::string, Value> str_keys;
            };
            std::vector<ThreadState> tstates(num_threads);

            std::vector<std::thread> threads;
            for (unsigned int t = 0; t < num_threads; t++) {
                threads.emplace_back([&, t]() {
                    auto &ts = tstates[t];
                    // Use the optimized FastCSVReader on a buffer slice.
                    FastCSVReader thread_reader(buffer, ranges[t], ranges[t + 1], delim);
                    DataChunk chunk;
                    chunk.Initialize(types);

                    while (true) {
                        chunk.Reset();
                        idx_t cnt = thread_reader.ReadChunk(chunk, types);
                        if (cnt == 0) break;

                        auto &gvec = chunk.GetVector(group_col);
                        auto gtid = gvec.GetType().id();
                        bool is_int = (gtid == LogicalTypeId::INTEGER || gtid == LogicalTypeId::BIGINT);

                        for (idx_t i = 0; i < cnt; i++) {
                            std::vector<AggState> *states_ptr = nullptr;
                            if (is_int) {
                                int64_t k = (gtid == LogicalTypeId::BIGINT)
                                    ? reinterpret_cast<const int64_t *>(gvec.GetData())[i]
                                    : (int64_t)reinterpret_cast<const int32_t *>(gvec.GetData())[i];
                                auto it = ts.int_groups.find(k);
                                if (it == ts.int_groups.end()) {
                                    ts.int_keys[k] = chunk.GetValue(group_col, i);
                                    it = ts.int_groups.emplace(k, std::vector<AggState>(num_aggs)).first;
                                }
                                states_ptr = &it->second;
                            } else {
                                auto &s = reinterpret_cast<const string_t *>(gvec.GetData())[i];
                                std::string k(s.GetData(), s.GetSize());
                                auto it = ts.str_groups.find(k);
                                if (it == ts.str_groups.end()) {
                                    ts.str_keys[k] = chunk.GetValue(group_col, i);
                                    it = ts.str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                                }
                                states_ptr = &it->second;
                            }

                            auto &states = *states_ptr;
                            for (idx_t a = 0; a < num_aggs; a++) {
                                auto &state = states[a];
                                auto &info = agg_infos[a];
                                if (info.name == "COUNT" && info.is_count_star) {
                                    state.count++;
                                } else if (info.col_idx != INVALID_INDEX) {
                                    auto &vec = chunk.GetVector(info.col_idx);
                                    if (vec.GetValidity().RowIsValid(i)) {
                                        if (info.name == "COUNT") {
                                            state.count++;
                                        } else {
                                            double val = ReadDouble(vec, i);
                                            if (info.name == "SUM" || info.name == "AVG") {
                                                state.count++; state.sum += val;
                                            } else if (info.name == "MIN") {
                                                if (!state.has_min || val < state.sum_min) {
                                                    state.sum_min = val; state.has_min = true;
                                                }
                                            } else if (info.name == "MAX") {
                                                if (!state.has_max || val > state.sum_max) {
                                                    state.sum_max = val; state.has_max = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                });
            }
            for (auto &th : threads) th.join();

            // Merge per-thread states.
            std::unordered_map<int64_t, std::vector<AggState>> int_groups;
            std::unordered_map<std::string, std::vector<AggState>> str_groups;
            std::vector<int64_t> int_order;
            std::vector<std::string> str_order;
            std::unordered_map<int64_t, Value> int_keys;
            std::unordered_map<std::string, Value> str_keys;

            for (auto &ts : tstates) {
                for (auto &kv : ts.int_groups) {
                    auto &dst = int_groups[kv.first];
                    if (dst.empty()) {
                        dst = std::move(kv.second);
                        int_keys[kv.first] = ts.int_keys[kv.first];
                        int_order.push_back(kv.first);
                    } else {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            dst[a].count += kv.second[a].count;
                            dst[a].sum += kv.second[a].sum;
                            if (kv.second[a].has_min &&
                                (!dst[a].has_min || kv.second[a].sum_min < dst[a].sum_min)) {
                                dst[a].sum_min = kv.second[a].sum_min; dst[a].has_min = true;
                            }
                            if (kv.second[a].has_max &&
                                (!dst[a].has_max || kv.second[a].sum_max > dst[a].sum_max)) {
                                dst[a].sum_max = kv.second[a].sum_max; dst[a].has_max = true;
                            }
                        }
                    }
                }
                for (auto &kv : ts.str_groups) {
                    auto &dst = str_groups[kv.first];
                    if (dst.empty()) {
                        dst = std::move(kv.second);
                        str_keys[kv.first] = ts.str_keys[kv.first];
                        str_order.push_back(kv.first);
                    } else {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            dst[a].count += kv.second[a].count;
                            dst[a].sum += kv.second[a].sum;
                            if (kv.second[a].has_min &&
                                (!dst[a].has_min || kv.second[a].sum_min < dst[a].sum_min)) {
                                dst[a].sum_min = kv.second[a].sum_min; dst[a].has_min = true;
                            }
                            if (kv.second[a].has_max &&
                                (!dst[a].has_max || kv.second[a].sum_max > dst[a].sum_max)) {
                                dst[a].sum_max = kv.second[a].sum_max; dst[a].has_max = true;
                            }
                        }
                    }
                }
            }

            for (auto k : int_order) {
                std::string sk = std::to_string(k);
                group_states[sk] = std::move(int_groups[k]);
                group_keys[sk] = {int_keys[k]};
                group_order.push_back(sk);
            }
            for (auto &k : str_order) {
                group_states[k] = std::move(str_groups[k]);
                group_keys[k] = {str_keys[k]};
                group_order.push_back(k);
            }
        } else if (false) { // marker to keep else-if structure intact
            // Initialize the file scan to load the buffer.
            file_scan_for_group->Init();
            auto *reader = file_scan_for_group->GetReader();
            const char *buffer = reader->GetBuffer();
            size_t total_size = reader->GetSize();
            size_t data_start = reader->GetPos(); // after header
            char delim = file_scan_for_group->GetDelimiter();

            idx_t group_col = group_col_indices[0];
            auto types = children[0]->GetTypes();

            // Determine number of threads based on file size.
            unsigned int num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0 || num_threads > 16) num_threads = 8;
            size_t data_size = total_size - data_start;
            if (data_size < 4 * 1024 * 1024) num_threads = 1;

            // Compute per-thread byte ranges aligned to line boundaries.
            std::vector<size_t> ranges(num_threads + 1);
            ranges[0] = data_start;
            ranges[num_threads] = total_size;
            for (unsigned int t = 1; t < num_threads; t++) {
                size_t target = data_start + (data_size * t) / num_threads;
                ranges[t] = FastCSVReader::FindLineStart(buffer, total_size, target);
            }

            // Per-thread state.
            struct ThreadState {
                std::unordered_map<int64_t, std::vector<AggState>> int_groups;
                std::unordered_map<std::string, std::vector<AggState>> str_groups;
                std::unordered_map<int64_t, Value> int_keys;
                std::unordered_map<std::string, Value> str_keys;
            };
            std::vector<ThreadState> tstates(num_threads);

            std::vector<std::thread> threads;
            for (unsigned int t = 0; t < num_threads; t++) {
                threads.emplace_back([&, t]() {
                    auto &ts = tstates[t];
                    size_t pos = ranges[t];
                    size_t end = ranges[t + 1];

                    DataChunk thread_chunk;
                    thread_chunk.Initialize(types);

                    // Inline mini-parser that reads from buffer[pos..end].
                    auto ParseField = [&](const char *&fs, size_t &fl) -> bool {
                        if (pos >= end) return false;
                        char c = buffer[pos];
                        if (c == '\n' || c == '\r') return false;
                        if (c == '"') {
                            pos++; fs = buffer + pos; size_t s = pos;
                            while (pos < end && buffer[pos] != '"') pos++;
                            fl = pos - s;
                            if (pos < end) pos++;
                            if (pos < end && buffer[pos] == delim) pos++;
                            return true;
                        }
                        fs = buffer + pos; size_t s = pos;
                        while (pos < end && buffer[pos] != delim &&
                               buffer[pos] != '\n' && buffer[pos] != '\r') pos++;
                        fl = pos - s;
                        if (pos < end && buffer[pos] == delim) pos++;
                        return true;
                    };

                    auto ParseInt64Local = [](const char *s, size_t len) -> int64_t {
                        if (len == 0) return 0;
                        bool neg = (s[0] == '-');
                        int64_t r = 0;
                        for (size_t i = neg ? 1 : 0; i < len; i++) r = r * 10 + (s[i] - '0');
                        return neg ? -r : r;
                    };

                    auto ParseDoubleLocal = [](const char *s, size_t len) -> double {
                        char buf[64];
                        size_t cl = len < 63 ? len : 63;
                        memcpy(buf, s, cl); buf[cl] = '\0';
                        return strtod(buf, nullptr);
                    };

                    idx_t num_cols = static_cast<idx_t>(types.size());
                    // Hoist field storage out of the loop — reused per row.
                    std::vector<const char*> field_starts(num_cols);
                    std::vector<size_t> field_lens(num_cols);
                    while (pos < end) {
                        idx_t col = 0;
                        const char *fs; size_t fl;
                        while (col < num_cols && ParseField(fs, fl)) {
                            field_starts[col] = fs;
                            field_lens[col] = fl;
                            col++;
                        }
                        // Skip rest of line.
                        while (pos < end && buffer[pos] != '\n' && buffer[pos] != '\r') pos++;
                        if (pos < end && buffer[pos] == '\r') pos++;
                        if (pos < end && buffer[pos] == '\n') pos++;
                        if (col == 0) continue;

                        // Determine group key from group_col field.
                        if (group_col >= col) continue;
                        const char *gf_ptr = field_starts[group_col];
                        size_t gf_len = field_lens[group_col];

                        std::vector<AggState> *states_ptr = nullptr;
                        auto gtid = types[group_col].id();
                        if (gtid == LogicalTypeId::BIGINT || gtid == LogicalTypeId::INTEGER) {
                            int64_t k = ParseInt64Local(gf_ptr, gf_len);
                            auto it = ts.int_groups.find(k);
                            if (it == ts.int_groups.end()) {
                                ts.int_keys[k] = (gtid == LogicalTypeId::BIGINT)
                                    ? Value::BIGINT(k) : Value::INTEGER((int32_t)k);
                                it = ts.int_groups.emplace(k, std::vector<AggState>(num_aggs)).first;
                            }
                            states_ptr = &it->second;
                        } else {
                            std::string k(gf_ptr, gf_len);
                            auto it = ts.str_groups.find(k);
                            if (it == ts.str_groups.end()) {
                                ts.str_keys[k] = Value::VARCHAR(k);
                                it = ts.str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                            }
                            states_ptr = &it->second;
                        }

                        auto &states = *states_ptr;
                        for (idx_t a = 0; a < num_aggs; a++) {
                            auto &state = states[a];
                            auto &info = agg_infos[a];
                            if (info.name == "COUNT" && info.is_count_star) {
                                state.count++;
                            } else if (info.col_idx != INVALID_INDEX && info.col_idx < col) {
                                const char *af_ptr = field_starts[info.col_idx];
                                size_t af_len = field_lens[info.col_idx];
                                if (af_len == 0) continue;
                                double val = 0;
                                auto cid = types[info.col_idx].id();
                                if (cid == LogicalTypeId::BIGINT)
                                    val = (double)ParseInt64Local(af_ptr, af_len);
                                else if (cid == LogicalTypeId::INTEGER)
                                    val = (double)(int32_t)ParseInt64Local(af_ptr, af_len);
                                else if (cid == LogicalTypeId::DOUBLE || cid == LogicalTypeId::FLOAT)
                                    val = ParseDoubleLocal(af_ptr, af_len);

                                if (info.name == "COUNT") {
                                    state.count++;
                                } else if (info.name == "SUM" || info.name == "AVG") {
                                    state.count++; state.sum += val;
                                } else if (info.name == "MIN") {
                                    if (!state.has_min || val < state.sum_min) {
                                        state.sum_min = val; state.has_min = true;
                                    }
                                } else if (info.name == "MAX") {
                                    if (!state.has_max || val > state.sum_max) {
                                        state.sum_max = val; state.has_max = true;
                                    }
                                }
                            }
                        }
                    }
                });
            }
            for (auto &th : threads) th.join();

            // Merge per-thread states.
            std::unordered_map<int64_t, std::vector<AggState>> int_groups;
            std::unordered_map<std::string, std::vector<AggState>> str_groups;
            std::vector<int64_t> int_order;
            std::vector<std::string> str_order;
            std::unordered_map<int64_t, Value> int_keys;
            std::unordered_map<std::string, Value> str_keys;

            for (auto &ts : tstates) {
                for (auto &kv : ts.int_groups) {
                    auto &dst = int_groups[kv.first];
                    if (dst.empty()) {
                        dst = std::move(kv.second);
                        int_keys[kv.first] = ts.int_keys[kv.first];
                        int_order.push_back(kv.first);
                    } else {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            dst[a].count += kv.second[a].count;
                            dst[a].sum += kv.second[a].sum;
                            if (kv.second[a].has_min &&
                                (!dst[a].has_min || kv.second[a].sum_min < dst[a].sum_min)) {
                                dst[a].sum_min = kv.second[a].sum_min; dst[a].has_min = true;
                            }
                            if (kv.second[a].has_max &&
                                (!dst[a].has_max || kv.second[a].sum_max > dst[a].sum_max)) {
                                dst[a].sum_max = kv.second[a].sum_max; dst[a].has_max = true;
                            }
                        }
                    }
                }
                for (auto &kv : ts.str_groups) {
                    auto &dst = str_groups[kv.first];
                    if (dst.empty()) {
                        dst = std::move(kv.second);
                        str_keys[kv.first] = ts.str_keys[kv.first];
                        str_order.push_back(kv.first);
                    } else {
                        for (idx_t a = 0; a < num_aggs; a++) {
                            dst[a].count += kv.second[a].count;
                            dst[a].sum += kv.second[a].sum;
                            if (kv.second[a].has_min &&
                                (!dst[a].has_min || kv.second[a].sum_min < dst[a].sum_min)) {
                                dst[a].sum_min = kv.second[a].sum_min; dst[a].has_min = true;
                            }
                            if (kv.second[a].has_max &&
                                (!dst[a].has_max || kv.second[a].sum_max > dst[a].sum_max)) {
                                dst[a].sum_max = kv.second[a].sum_max; dst[a].has_max = true;
                            }
                        }
                    }
                }
            }

            // Build result map for downstream.
            for (auto k : int_order) {
                std::string sk = std::to_string(k);
                group_states[sk] = std::move(int_groups[k]);
                group_keys[sk] = {int_keys[k]};
                group_order.push_back(sk);
            }
            for (auto &k : str_order) {
                group_states[k] = std::move(str_groups[k]);
                group_keys[k] = {str_keys[k]};
                group_order.push_back(k);
            }
        } else if (single_group_fast) {
            idx_t group_col = group_col_indices[0];

            // Column pruning: tell file scan to only parse needed columns.
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                std::vector<bool> needed(children[0]->GetTypes().size(), false);
                needed[group_col] = true;
                for (auto &info : agg_infos) {
                    if (info.col_idx != INVALID_INDEX && info.col_idx < needed.size()) {
                        needed[info.col_idx] = true;
                    }
                }
                fs->SetProjection(std::move(needed));
            }

            // Two hash maps based on group column type — avoid string conversion.
            std::unordered_map<int64_t, std::vector<AggState>> int_groups;
            std::unordered_map<std::string, std::vector<AggState>> str_groups;
            std::vector<int64_t> int_order;
            std::vector<std::string> str_order;
            std::unordered_map<int64_t, Value> int_keys;
            std::unordered_map<std::string, Value> str_keys;

            while (children[0]->GetData(chunk)) {
                idx_t chunk_size = chunk.size();
                auto &gvec = chunk.GetVector(group_col);
                auto gtid = gvec.GetType().id();
                bool is_int = (gtid == LogicalTypeId::INTEGER || gtid == LogicalTypeId::BIGINT);
                bool is_str = (gtid == LogicalTypeId::VARCHAR);

                for (idx_t i = 0; i < chunk_size; i++) {
                    std::vector<AggState> *states_ptr = nullptr;

                    if (is_int) {
                        int64_t k = (gtid == LogicalTypeId::BIGINT)
                            ? reinterpret_cast<const int64_t *>(gvec.GetData())[i]
                            : static_cast<int64_t>(reinterpret_cast<const int32_t *>(gvec.GetData())[i]);
                        auto it = int_groups.find(k);
                        if (it == int_groups.end()) {
                            int_order.push_back(k);
                            int_keys[k] = chunk.GetValue(group_col, i);
                            it = int_groups.emplace(k, std::vector<AggState>(num_aggs)).first;
                        }
                        states_ptr = &it->second;
                    } else if (is_str) {
                        auto &s = reinterpret_cast<const string_t *>(gvec.GetData())[i];
                        std::string k(s.GetData(), s.GetSize());
                        auto it = str_groups.find(k);
                        if (it == str_groups.end()) {
                            str_order.push_back(k);
                            str_keys[k] = chunk.GetValue(group_col, i);
                            it = str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                        }
                        states_ptr = &it->second;
                    } else {
                        // Fallback to string key for other types.
                        std::string k = chunk.GetValue(group_col, i).ToString();
                        auto it = str_groups.find(k);
                        if (it == str_groups.end()) {
                            str_order.push_back(k);
                            str_keys[k] = chunk.GetValue(group_col, i);
                            it = str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                        }
                        states_ptr = &it->second;
                    }

                    auto &states = *states_ptr;
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &state = states[a];
                        auto &info = agg_infos[a];
                        if (info.name == "COUNT" && info.is_count_star) {
                            state.count++;
                        } else if (info.col_idx != INVALID_INDEX) {
                            auto &vec = chunk.GetVector(info.col_idx);
                            if (vec.GetValidity().RowIsValid(i)) {
                                if (info.name == "COUNT") {
                                    state.count++;
                                } else {
                                    double val = ReadDouble(vec, i);
                                    if (info.name == "SUM" || info.name == "AVG") {
                                        state.count++;
                                        state.sum += val;
                                    } else if (info.name == "MIN") {
                                        if (!state.has_min || val < state.sum_min) {
                                            state.sum_min = val; state.has_min = true;
                                            state.min_val = chunk.GetValue(info.col_idx, i);
                                        }
                                    } else if (info.name == "MAX") {
                                        if (!state.has_max || val > state.sum_max) {
                                            state.sum_max = val; state.has_max = true;
                                            state.max_val = chunk.GetValue(info.col_idx, i);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                total_rows_processed += chunk_size;
            }

            // Move into the standard group_states map for result building.
            for (auto k : int_order) {
                std::string sk = std::to_string(k);
                group_states[sk] = std::move(int_groups[k]);
                group_keys[sk] = {int_keys[k]};
                group_order.push_back(sk);
            }
            for (auto &k : str_order) {
                group_states[k] = std::move(str_groups[k]);
                group_keys[k] = {str_keys[k]};
                group_order.push_back(k);
            }
        } else if (is_simple_count_star) {
            // Ultra-fast: if child is a file scan, just count newlines.
            group_order.push_back("");
            group_states[""].resize(1);
            group_keys[""] = {};
            auto &state = group_states[""][0];

            auto *file_scan = dynamic_cast<PhysicalFileScan *>(children[0].get());
            auto *count_scan = dynamic_cast<PhysicalCountScan *>(children[0].get());
            if (file_scan) {
                // Reuse the already-loaded reader — no second fread.
                auto *r = file_scan->GetReader();
                idx_t count = r->CountRows();
                state.count = static_cast<int64_t>(count);
                total_rows_processed = count;
            } else if (count_scan) {
                state.count = static_cast<int64_t>(count_scan->GetRowCount());
                total_rows_processed = count_scan->GetRowCount();
            } else {
                // Regular table — just count chunk sizes.
                while (children[0]->GetData(chunk)) {
                    state.count += static_cast<int64_t>(chunk.size());
                    total_rows_processed += chunk.size();
                }
            }
        } else if (all_simple_aggs && is_simple_no_group) {
            // Fast: no GROUP BY, simple aggregates — process vectors directly, no key building.
            // Column pruning: only parse columns we aggregate over.
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                std::vector<bool> needed(children[0]->GetTypes().size(), false);
                for (auto &info : agg_infos) {
                    if (info.col_idx != INVALID_INDEX && info.col_idx < needed.size()) {
                        needed[info.col_idx] = true;
                    }
                }
                fs->SetProjection(std::move(needed));
            }

            group_order.push_back("");
            group_states[""].resize(num_aggs);
            group_keys[""] = {};
            auto &states = group_states[""];

            while (children[0]->GetData(chunk)) {
                idx_t chunk_size = chunk.size();
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];
                    if (info.name == "COUNT" && info.is_count_star) {
                        state.count += static_cast<int64_t>(chunk_size);
                    } else if (info.col_idx != INVALID_INDEX) {
                        auto &vec = chunk.GetVector(info.col_idx);
                        auto *validity = &vec.GetValidity();
                        bool all_valid = (validity->GetData() == nullptr); // nullptr = all valid
                        if (info.name == "COUNT") {
                            if (all_valid) { state.count += static_cast<int64_t>(chunk_size); }
                            else { for (idx_t i = 0; i < chunk_size; i++) { if (validity->RowIsValid(i)) state.count++; } }
                        } else if (info.name == "SUM" || info.name == "AVG") {
                            auto tid = vec.GetType().id();
                            if (tid == LogicalTypeId::BIGINT && all_valid) {
                                auto *arr = reinterpret_cast<const int64_t *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += static_cast<double>(arr[i]);
                                state.count += static_cast<int64_t>(chunk_size);
                            } else if (tid == LogicalTypeId::DOUBLE && all_valid) {
                                auto *arr = reinterpret_cast<const double *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += arr[i];
                                state.count += static_cast<int64_t>(chunk_size);
                            } else if (tid == LogicalTypeId::INTEGER && all_valid) {
                                auto *arr = reinterpret_cast<const int32_t *>(vec.GetData());
                                for (idx_t i = 0; i < chunk_size; i++) state.sum += static_cast<double>(arr[i]);
                                state.count += static_cast<int64_t>(chunk_size);
                            } else {
                                for (idx_t i = 0; i < chunk_size; i++) {
                                    if (all_valid || validity->RowIsValid(i)) {
                                        state.sum += ReadDouble(vec, i);
                                        state.count++;
                                    }
                                }
                            }
                        } else if (info.name == "MIN" || info.name == "MAX") {
                            for (idx_t i = 0; i < chunk_size; i++) {
                                if (all_valid || validity->RowIsValid(i)) {
                                    double val = ReadDouble(vec, i);
                                    if (info.name == "MIN") {
                                        if (!state.has_min || val < state.sum_min) {
                                            state.sum_min = val; state.has_min = true;
                                            state.min_val = chunk.GetValue(info.col_idx, i);
                                        }
                                    } else {
                                        if (!state.has_max || val > state.sum_max) {
                                            state.sum_max = val; state.has_max = true;
                                            state.max_val = chunk.GetValue(info.col_idx, i);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                total_rows_processed += chunk_size;
            }
        } else

        while (children[0]->GetData(chunk)) {

            idx_t chunk_size = chunk.size();
            for (idx_t i = 0; i < chunk_size; i++) {
                // Build group key directly from vectors.
                key.clear();
                std::vector<Value> key_vals;
                for (auto gc : group_col_indices) {
                    AppendGroupKey(key, chunk.GetVector(gc), i);
                    key_vals.push_back(chunk.GetValue(gc, i));
                }

                if (group_states.find(key) == group_states.end()) {
                    group_states[key].resize(num_aggs);
                    group_keys[key] = key_vals;
                    group_order.push_back(key);
                }

                auto &states = group_states[key];

                // Update aggregates — read directly from vectors.
                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];

                    if (info.name == "COUNT") {
                        if (info.is_count_star) {
                            state.count++;
                        } else if (info.col_idx != INVALID_INDEX &&
                                   chunk.GetVector(info.col_idx).GetValidity().RowIsValid(i)) {
                            if (info.is_distinct) {
                                auto v = chunk.GetValue(info.col_idx, i);
                                if (state.distinct_set.insert(v.ToString()).second)
                                    state.count++;
                            } else {
                                state.count++;
                            }
                        }
                    } else if (info.col_idx != INVALID_INDEX) {
                        auto &vec = chunk.GetVector(info.col_idx);
                        if (vec.GetValidity().RowIsValid(i)) {
                            double val = ReadDouble(vec, i);
                            if (info.name == "SUM" || info.name == "AVG") {
                                state.count++;
                                state.sum += val;
                            } else if (info.name == "MIN") {
                                if (!state.has_min || val < state.sum_min) {
                                    state.sum_min = val;
                                    state.has_min = true;
                                    state.min_val = chunk.GetValue(info.col_idx, i);
                                }
                            } else if (info.name == "MAX") {
                                if (!state.has_max || val > state.sum_max) {
                                    state.sum_max = val;
                                    state.has_max = true;
                                    state.max_val = chunk.GetValue(info.col_idx, i);
                                }
                            } else if (info.name == "STDDEV" || info.name == "STDDEV_SAMP" ||
                                       info.name == "STDDEV_POP" || info.name == "VARIANCE" ||
                                       info.name == "VAR_SAMP" || info.name == "VAR_POP") {
                                state.count++;
                                state.sum += val;
                                state.sum_sq += val * val;
                            } else if (info.name == "MEDIAN") {
                                state.values.push_back(val);
                            } else if (info.name == "BOOL_AND") {
                                state.count++;
                                state.bool_and = state.bool_and && (val != 0.0);
                            } else if (info.name == "BOOL_OR") {
                                state.count++;
                                state.bool_or = state.bool_or || (val != 0.0);
                            } else if (info.name == "STRING_AGG" || info.name == "LISTAGG" || info.name == "GROUP_CONCAT") {
                                auto v = chunk.GetValue(info.col_idx, i);
                                if (!v.IsNull()) {
                                    if (state.str_started) state.str_agg += state.str_delim.empty() ? "," : state.str_delim;
                                    state.str_agg += v.ToString();
                                    state.str_started = true;
                                }
                            }
                        }
                    } else {
                        // Complex expression — fallback to Value path.
                        auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                        Value arg_val;
                        if (!agg_expr.arguments.empty()) {
                            DataChunk row_chunk;
                            row_chunk.Initialize(children[0]->GetTypes());
                            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                                row_chunk.SetValue(c, 0, chunk.GetValue(c, i));
                            row_chunk.SetCardinality(1);
                            Vector res(agg_expr.arguments[0]->GetReturnType());
                            ExpressionExecutor::Execute(*agg_expr.arguments[0], row_chunk, res, 1);
                            arg_val = res.GetValue(0);
                        }
                        if (info.name == "COUNT" && !arg_val.IsNull()) state.count++;
                        else if ((info.name == "SUM" || info.name == "AVG") && !arg_val.IsNull()) {
                            state.count++;
                            state.sum += arg_val.GetValue<double>();
                        }
                    }
                }
            }
            total_rows_processed += chunk_size;
        }
        // Build result rows.
        for (auto &gk : group_order) {
            auto &key_vals = group_keys[gk];
            auto &states = group_states[gk];

            std::vector<Value> result_row;

            // Group by columns first.
            for (auto &v : key_vals) {
                result_row.push_back(v);
            }

            // Aggregate results.
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                auto name = StringUtil::Upper(agg_expr.function_name);
                auto &state = states[a];

                if (name == "COUNT") {
                    result_row.push_back(Value::BIGINT(state.count));
                } else if (name == "SUM") {
                    auto ret_type = agg_expr.GetReturnType().id();
                    if (ret_type == LogicalTypeId::BIGINT) {
                        result_row.push_back(Value::BIGINT(static_cast<int64_t>(state.sum)));
                    } else {
                        result_row.push_back(Value::DOUBLE(state.sum));
                    }
                } else if (name == "AVG") {
                    if (state.count > 0) {
                        result_row.push_back(Value::DOUBLE(state.sum / state.count));
                    } else {
                        result_row.push_back(Value());
                    }
                } else if (name == "MIN") {
                    result_row.push_back(state.has_min ? state.min_val : Value());
                } else if (name == "MAX") {
                    result_row.push_back(state.has_max ? state.max_val : Value());
                } else if (name == "STRING_AGG" || name == "LISTAGG" || name == "GROUP_CONCAT") {
                    result_row.push_back(state.str_started ? Value::VARCHAR(state.str_agg) : Value());
                } else if (name == "STDDEV" || name == "STDDEV_SAMP") {
                    if (state.count > 1) {
                        double mean = state.sum / state.count;
                        double var = (state.sum_sq - state.count * mean * mean) / (state.count - 1);
                        result_row.push_back(Value::DOUBLE(std::sqrt(var)));
                    } else {
                        result_row.push_back(Value());
                    }
                } else if (name == "STDDEV_POP") {
                    if (state.count > 0) {
                        double mean = state.sum / state.count;
                        double var = (state.sum_sq - state.count * mean * mean) / state.count;
                        result_row.push_back(Value::DOUBLE(std::sqrt(var)));
                    } else {
                        result_row.push_back(Value());
                    }
                } else if (name == "VARIANCE" || name == "VAR_SAMP") {
                    if (state.count > 1) {
                        double mean = state.sum / state.count;
                        double var = (state.sum_sq - state.count * mean * mean) / (state.count - 1);
                        result_row.push_back(Value::DOUBLE(var));
                    } else {
                        result_row.push_back(Value());
                    }
                } else if (name == "VAR_POP") {
                    if (state.count > 0) {
                        double mean = state.sum / state.count;
                        double var = (state.sum_sq - state.count * mean * mean) / state.count;
                        result_row.push_back(Value::DOUBLE(var));
                    } else {
                        result_row.push_back(Value());
                    }
                } else if (name == "MEDIAN") {
                    if (!state.values.empty()) {
                        std::sort(state.values.begin(), state.values.end());
                        size_t mid = state.values.size() / 2;
                        double median = (state.values.size() % 2 == 0)
                            ? (state.values[mid - 1] + state.values[mid]) / 2.0
                            : state.values[mid];
                        result_row.push_back(Value::DOUBLE(median));
                    } else {
                        result_row.push_back(Value());
                    }
                } else if (name == "BOOL_AND") {
                    result_row.push_back(state.count > 0 ? Value::BOOLEAN(state.bool_and) : Value());
                } else if (name == "BOOL_OR") {
                    result_row.push_back(state.count > 0 ? Value::BOOLEAN(state.bool_or) : Value());
                }
            }

            result_rows_.push_back(std::move(result_row));
        }

        // Handle no-group aggregation (e.g., SELECT COUNT(*) FROM t).
        if (groups_.empty() && result_rows_.empty()) {
            std::vector<Value> row;
            // Use default states (count=0, etc.)
            std::vector<AggState> default_states(num_aggs);

            // We still need to process all rows for no-group aggs.
            // They were already processed above, but if there are no rows
            // and no groups, we need a single result row with default values.
            for (idx_t a = 0; a < num_aggs; a++) {
                auto &agg_expr = static_cast<BoundFunction &>(*aggregates_[a]);
                auto name = StringUtil::Upper(agg_expr.function_name);
                if (name == "COUNT") {
                    row.push_back(Value::BIGINT(0));
                } else {
                    row.push_back(Value()); // NULL
                }
            }
            result_rows_.push_back(std::move(row));
        }
    }

    std::vector<BoundExprPtr> groups_;
    std::vector<BoundExprPtr> aggregates_;
    std::vector<std::vector<Value>> result_rows_;
    bool computed_ = false;
    idx_t emit_pos_ = 0;
};

// ============================================================================
// Hash Join
// ============================================================================

class PhysicalHashJoin : public PhysicalOperator {
public:
    PhysicalHashJoin(JoinType join_type, BoundExprPtr condition,
                     std::vector<LogicalType> result_types,
                     idx_t left_col_count, idx_t right_col_count)
        : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(result_types)),
          join_type_(join_type), condition_(std::move(condition)),
          left_col_count_(left_col_count), right_col_count_(right_col_count) {}

    void Init() override {
        for (auto &child : children) child->Init();
        built_ = false;
        emit_pos_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!built_) {
            BuildAndProbe();
            built_ = true;
        }

        if (emit_pos_ >= result_rows_.size()) return false;

        result.Initialize(GetTypes());
        idx_t count = 0;
        while (emit_pos_ < result_rows_.size() && count < VECTOR_SIZE) {
            auto &row = result_rows_[emit_pos_];
            for (idx_t col = 0; col < row.size(); col++) {
                result.SetValue(col, count, row[col]);
            }
            emit_pos_++;
            count++;
        }
        result.SetCardinality(count);
        return count > 0;
    }

private:
    void BuildAndProbe() {
        // Collect all rows from both sides.
        std::vector<std::vector<Value>> left_rows, right_rows;

        DataChunk chunk;
        while (true) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                    row.push_back(chunk.GetValue(col, i));
                left_rows.push_back(std::move(row));
            }
        }

        while (true) {
            chunk.Initialize(children[1]->GetTypes());
            if (!children[1]->GetData(chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                    row.push_back(chunk.GetValue(col, i));
                right_rows.push_back(std::move(row));
            }
        }

        // Extract join column indices from condition.
        // Column indices are combined (left+right). Convert right to local index.
        idx_t left_join_col = 0, right_join_col = 0;
        if (condition_ && condition_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*condition_);
            if (cmp.left->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                left_join_col = static_cast<BoundColumnRef &>(*cmp.left).column_index;
            }
            if (cmp.right->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                idx_t combined = static_cast<BoundColumnRef &>(*cmp.right).column_index;
                // If combined index >= left_col_count, it's a right table column.
                if (combined >= left_col_count_) {
                    right_join_col = combined - left_col_count_;
                } else {
                    right_join_col = combined;
                }
            }
        }

        // Build hash table on right side.
        std::unordered_map<std::string, std::vector<idx_t>> hash_table;
        for (idx_t i = 0; i < right_rows.size(); i++) {
            auto key = right_rows[i][right_join_col].ToString();
            hash_table[key].push_back(i);
        }

        // Probe with left side.
        std::vector<bool> right_matched(right_rows.size(), false);

        for (auto &left_row : left_rows) {
            auto key = left_row[left_join_col].ToString();
            auto it = hash_table.find(key);
            bool matched = false;

            if (it != hash_table.end()) {
                for (auto right_idx : it->second) {
                    // Emit combined row.
                    std::vector<Value> combined;
                    for (auto &v : left_row) combined.push_back(v);
                    for (auto &v : right_rows[right_idx]) combined.push_back(v);
                    result_rows_.push_back(std::move(combined));
                    right_matched[right_idx] = true;
                    matched = true;
                }
            }

            // LEFT/FULL: emit left row with NULLs for right side.
            if (!matched && (join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL)) {
                std::vector<Value> combined;
                for (auto &v : left_row) combined.push_back(v);
                for (idx_t c = 0; c < right_col_count_; c++) combined.push_back(Value());
                result_rows_.push_back(std::move(combined));
            }
        }

        // RIGHT/FULL: emit unmatched right rows.
        if (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL) {
            for (idx_t i = 0; i < right_rows.size(); i++) {
                if (!right_matched[i]) {
                    std::vector<Value> combined;
                    for (idx_t c = 0; c < left_col_count_; c++) combined.push_back(Value());
                    for (auto &v : right_rows[i]) combined.push_back(v);
                    result_rows_.push_back(std::move(combined));
                }
            }
        }

        // CROSS: handled above when condition is null (all pairs match).
        if (join_type_ == JoinType::CROSS && result_rows_.empty()) {
            for (auto &left_row : left_rows) {
                for (auto &right_row : right_rows) {
                    std::vector<Value> combined;
                    for (auto &v : left_row) combined.push_back(v);
                    for (auto &v : right_row) combined.push_back(v);
                    result_rows_.push_back(std::move(combined));
                }
            }
        }
    }

    JoinType join_type_;
    BoundExprPtr condition_;
    idx_t left_col_count_;
    idx_t right_col_count_;
    std::vector<std::vector<Value>> result_rows_;
    bool built_ = false;
    idx_t emit_pos_ = 0;
};

// ============================================================================
// Update
// ============================================================================

class PhysicalUpdate : public PhysicalOperator {
public:
    PhysicalUpdate(TableCatalogEntry *table,
                   std::vector<BoundUpdateAssignment> assignments,
                   BoundExprPtr where_clause)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), assignments_(std::move(assignments)),
          where_clause_(std::move(where_clause)) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        auto &storage = table_->GetStorage();

        // Scan all data, modify matching rows, rebuild storage.
        std::vector<std::vector<Value>> all_rows;
        auto state = storage.InitScan();
        DataChunk chunk;
        while (true) {
            chunk.Initialize(types);
            if (!storage.Scan(state, chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                    row.push_back(chunk.GetValue(c, i));
                all_rows.push_back(std::move(row));
            }
        }

        // Create new storage and re-insert.
        auto new_storage = std::make_shared<DataTable>(types);

        for (auto &row : all_rows) {
            // Check WHERE.
            bool matches = true;
            if (where_clause_) {
                DataChunk single;
                single.Initialize(types);
                for (idx_t c = 0; c < row.size(); c++)
                    single.SetValue(c, 0, row[c]);
                Vector filter_result(LogicalType::BOOLEAN());
                ExpressionExecutor::Execute(*where_clause_, single, filter_result, 1);
                matches = filter_result.GetValidity().RowIsValid(0) &&
                          filter_result.GetData<bool>()[0];
            }

            if (matches) {
                // Apply assignments — evaluate against current row.
                DataChunk row_chunk;
                row_chunk.Initialize(types);
                for (idx_t c = 0; c < row.size(); c++)
                    row_chunk.SetValue(c, 0, row[c]);

                for (auto &assign : assignments_) {
                    if (assign.value->GetExpressionType() == BoundExpressionType::CONSTANT) {
                        row[assign.column_index] = ExpressionExecutor::ExecuteScalar(*assign.value);
                    } else {
                        Vector res(assign.value->GetReturnType());
                        ExpressionExecutor::Execute(*assign.value, row_chunk, res, 1);
                        row[assign.column_index] = res.GetValue(0);
                    }
                }
            }

            DataChunk insert_chunk;
            insert_chunk.Initialize(types);
            for (idx_t c = 0; c < row.size(); c++)
                insert_chunk.SetValue(c, 0, row[c]);
            new_storage->Append(insert_chunk);
        }

        table_->SetStorage(new_storage);
        return false;
    }

private:
    TableCatalogEntry *table_;
    std::vector<BoundUpdateAssignment> assignments_;
    BoundExprPtr where_clause_;
    bool done_ = false;
};

// ============================================================================
// Delete
// ============================================================================

class PhysicalDelete : public PhysicalOperator {
public:
    PhysicalDelete(TableCatalogEntry *table, BoundExprPtr where_clause)
        : PhysicalOperator(PhysicalOperatorType::INSERT, {}),
          table_(table), where_clause_(std::move(where_clause)) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;

        auto types = table_->GetTypes();
        auto &storage = table_->GetStorage();

        // Scan all data, keep non-matching rows.
        auto new_storage = std::make_shared<DataTable>(types);
        auto state = storage.InitScan();
        DataChunk chunk;

        while (true) {
            chunk.Initialize(types);
            if (!storage.Scan(state, chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                bool should_delete = false;
                if (where_clause_) {
                    DataChunk single;
                    single.Initialize(types);
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        single.SetValue(c, 0, chunk.GetValue(c, i));
                    Vector filter_result(LogicalType::BOOLEAN());
                    ExpressionExecutor::Execute(*where_clause_, single, filter_result, 1);
                    should_delete = filter_result.GetValidity().RowIsValid(0) &&
                                    filter_result.GetData<bool>()[0];
                } else {
                    should_delete = true; // DELETE without WHERE = delete all.
                }

                if (!should_delete) {
                    DataChunk keep;
                    keep.Initialize(types);
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        keep.SetValue(c, 0, chunk.GetValue(c, i));
                    new_storage->Append(keep);
                }
            }
        }

        table_->SetStorage(new_storage);
        return false;
    }

private:
    TableCatalogEntry *table_;
    BoundExprPtr where_clause_;
    bool done_ = false;
};

class PhysicalDummyScan : public PhysicalOperator {
public:
    PhysicalDummyScan()
        : PhysicalOperator(PhysicalOperatorType::DUMMY_SCAN, {}) {}

    void Init() override { done_ = false; }

    bool GetData(DataChunk &result) override {
        if (done_) return false;
        done_ = true;
        // Return a single empty row — projections above will fill in constants.
        result.Initialize({});
        result.SetCardinality(1);
        return true;
    }

private:
    bool done_ = false;
};

// ============================================================================
// Physical Planner
// ============================================================================

PhysicalOpPtr PhysicalPlanner::Plan(const LogicalOperator &logical) {
    switch (logical.GetOperatorType()) {
    case LogicalOperatorType::GET:
        return PlanGet(static_cast<const LogicalGet &>(logical));
    case LogicalOperatorType::FILTER:
        return PlanFilter(static_cast<const LogicalFilter &>(logical));
    case LogicalOperatorType::PROJECTION:
        return PlanProjection(static_cast<const LogicalProjection &>(logical));
    case LogicalOperatorType::ORDER_BY:
        return PlanOrderBy(static_cast<const LogicalOrderBy &>(logical));
    case LogicalOperatorType::LIMIT:
        return PlanLimit(static_cast<const LogicalLimit &>(logical));
    case LogicalOperatorType::INSERT:
        return PlanInsert(static_cast<const LogicalInsert &>(logical));
    case LogicalOperatorType::CREATE_TABLE:
        return PlanCreateTable(static_cast<const LogicalCreateTable &>(logical));
    case LogicalOperatorType::DROP_TABLE:
        return PlanDropTable(static_cast<const LogicalDropTable &>(logical));
    case LogicalOperatorType::UPDATE:
        return PlanUpdateOp(static_cast<const LogicalUpdate &>(logical));
    case LogicalOperatorType::DELETE_STMT:
        return PlanDeleteOp(static_cast<const LogicalDeleteOp &>(logical));
    case LogicalOperatorType::WINDOW:
        return PlanWindow(static_cast<const LogicalWindow &>(logical));
    case LogicalOperatorType::DISTINCT:
        return PlanDistinct(static_cast<const LogicalDistinct &>(logical));
    case LogicalOperatorType::AGGREGATE:
        return PlanAggregate(static_cast<const LogicalAggregate &>(logical));
    case LogicalOperatorType::JOIN:
        return PlanJoin(static_cast<const LogicalJoin &>(logical));
    case LogicalOperatorType::DUMMY_SCAN:
        return PlanDummyScan(static_cast<const LogicalDummyScan &>(logical));
    default:
        throw InternalException("Unknown logical operator type");
    }
}

PhysicalOpPtr PhysicalPlanner::PlanGet(const LogicalGet &op) {
    // Parquet streaming temporarily disabled — reverted while fixing regression.
    if (op.table && op.table->IsFileScan() && op.table->GetFileFormat() != "parquet") {
        return std::make_unique<PhysicalFileScan>(
            op.table->GetFilePath(), op.table->GetFileDelimiter(),
            op.table->GetTypes());
    }
    return std::make_unique<PhysicalTableScan>(op.table);
}

PhysicalOpPtr PhysicalPlanner::PlanFilter(const LogicalFilter &op) {
    auto &mutable_op = const_cast<LogicalFilter &>(op);
    auto result = std::make_unique<PhysicalFilter>(
        std::move(mutable_op.condition), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanProjection(const LogicalProjection &op) {
    auto &mutable_op = const_cast<LogicalProjection &>(op);
    auto result = std::make_unique<PhysicalProjection>(
        std::move(mutable_op.expressions), op.GetTypes());
    if (!op.children.empty()) {
        result->children.push_back(Plan(*op.children[0]));
    }
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanOrderBy(const LogicalOrderBy &op) {
    auto &mutable_op = const_cast<LogicalOrderBy &>(op);
    auto result = std::make_unique<PhysicalOrderBy>(
        std::move(mutable_op.orders), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanLimit(const LogicalLimit &op) {
    auto result = std::make_unique<PhysicalLimit>(
        op.limit_count, op.offset_count, op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanInsert(const LogicalInsert &op) {
    auto &mutable_op = const_cast<LogicalInsert &>(op);
    return std::make_unique<PhysicalInsert>(op.table, std::move(mutable_op.values));
}

PhysicalOpPtr PhysicalPlanner::PlanCreateTable(const LogicalCreateTable &op) {
    return std::make_unique<PhysicalCreateTable>(
        catalog_, op.table_name, op.columns, op.if_not_exists);
}

PhysicalOpPtr PhysicalPlanner::PlanDropTable(const LogicalDropTable &op) {
    return std::make_unique<PhysicalDropTable>(catalog_, op.table_name, op.if_exists);
}

PhysicalOpPtr PhysicalPlanner::PlanUpdateOp(const LogicalUpdate &op) {
    auto &mutable_op = const_cast<LogicalUpdate &>(op);
    return std::make_unique<PhysicalUpdate>(
        op.table, std::move(mutable_op.assignments), std::move(mutable_op.where_clause));
}

PhysicalOpPtr PhysicalPlanner::PlanDeleteOp(const LogicalDeleteOp &op) {
    auto &mutable_op = const_cast<LogicalDeleteOp &>(op);
    return std::make_unique<PhysicalDelete>(op.table, std::move(mutable_op.where_clause));
}

PhysicalOpPtr PhysicalPlanner::PlanWindow(const LogicalWindow &op) {
    auto &mutable_op = const_cast<LogicalWindow &>(op);
    auto result = std::make_unique<PhysicalWindow>(
        std::move(mutable_op.select_list), std::move(mutable_op.qualify), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanDistinct(const LogicalDistinct &op) {
    auto result = std::make_unique<PhysicalDistinct>(op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanAggregate(const LogicalAggregate &op) {
    auto &mutable_op = const_cast<LogicalAggregate &>(op);
    auto result = std::make_unique<PhysicalHashAggregate>(
        std::move(mutable_op.groups), std::move(mutable_op.aggregates), op.GetTypes());
    result->children.push_back(Plan(*op.children[0]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanJoin(const LogicalJoin &op) {
    auto &mutable_op = const_cast<LogicalJoin &>(op);
    idx_t left_cols = op.children[0]->GetTypes().size();
    idx_t right_cols = op.children[1]->GetTypes().size();
    auto result = std::make_unique<PhysicalHashJoin>(
        op.join_type, std::move(mutable_op.condition), op.GetTypes(),
        left_cols, right_cols);
    result->children.push_back(Plan(*op.children[0]));
    result->children.push_back(Plan(*op.children[1]));
    return result;
}

PhysicalOpPtr PhysicalPlanner::PlanDummyScan(const LogicalDummyScan &op) {
    return std::make_unique<PhysicalDummyScan>();
}

} // namespace slothdb

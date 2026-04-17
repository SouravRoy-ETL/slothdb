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
#include <iostream>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

namespace slothdb {

// Forward declarations — classes that reference each other across definitions.
class PhysicalHashJoin;

// Defined after PhysicalHashJoin's full declaration. Returns nullptr if op is not one.
static PhysicalHashJoin *AsHashJoin(PhysicalOperator *op);

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
    const std::vector<bool> &GetProjection() const { return projection_; }

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
        rg_size_ = 0;
        current_cols_.clear();
    }

    bool GetData(DataChunk &result) override {
        if (result.ColumnCount() != GetTypes().size()) result.Initialize(GetTypes());
        else result.Reset();

        idx_t num_cols = static_cast<idx_t>(GetTypes().size());

        // Load next row group's columns when current is exhausted.
        if (current_cols_.empty() || chunks_in_rg_ >= rg_size_) {
            if (rg_pos_ >= reader_->NumRowGroups()) return false;
            auto &rg_meta = reader_->GetMeta().row_groups[rg_pos_];
            rg_size_ = static_cast<idx_t>(rg_meta.num_rows);
            current_cols_.assign(num_cols, {});
            for (idx_t c = 0; c < num_cols; c++) {
                if (!projection_.empty() && c < projection_.size() && !projection_[c]) continue;
                current_cols_[c] = reader_->ReadColumn(rg_pos_, c);
            }
            rg_pos_++;
            chunks_in_rg_ = 0;
        }

        idx_t remaining = rg_size_ - chunks_in_rg_;
        idx_t count = std::min<idx_t>(remaining, VECTOR_SIZE);
        for (idx_t r = 0; r < count; r++) {
            for (idx_t c = 0; c < num_cols; c++) {
                if (!projection_.empty() && c < projection_.size() && !projection_[c]) {
                    result.GetVector(c).GetValidity().SetInvalid(r);
                    continue;
                }
                if (c < current_cols_.size() && chunks_in_rg_ + r < current_cols_[c].size()) {
                    result.SetValue(c, r, current_cols_[c][chunks_in_rg_ + r]);
                } else {
                    result.GetVector(c).GetValidity().SetInvalid(r);
                }
            }
        }
        result.SetCardinality(count);
        chunks_in_rg_ += count;
        return count > 0;
    }

    void SetProjection(std::vector<bool> mask) { projection_ = std::move(mask); }
    void SetNeededOutputs(const std::vector<bool> &mask) override { projection_ = mask; }
    const std::string &GetFilePath() const { return file_path_; }
    ParquetReader *GetReader() { return reader_.get(); }

private:
    std::string file_path_;
    std::unique_ptr<ParquetReader> reader_;
    idx_t rg_pos_ = 0;
    idx_t chunks_in_rg_ = 0;
    idx_t rg_size_ = 0;
    // One column's worth of Values per entry; empty if not projected.
    std::vector<std::vector<Value>> current_cols_;
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
        // Push LIMIT hint down so partial-sort / early-cut optimizations kick in.
        if (limit_count_ >= 0 && !children.empty()) {
            idx_t total = static_cast<idx_t>(limit_count_) + static_cast<idx_t>(offset_count_);
            children[0]->SetRowLimit(total);
        }
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

    void SetRowLimit(idx_t n) override { row_limit_ = n; }

    void Init() override {
        for (auto &child : children) child->Init();
        setup_done_ = false;
        use_fallback_ = false;
        emit_part_ = 0;
        emit_pos_ = 0;
        rank_cur_ = 1;
        rank_prev_ = INVALID_INDEX;
        dense_cur_ = 1;
        dense_prev_ = INVALID_INDEX;
        input_ = InputBuf{};
        partitions_.clear();
        select_win_info_.clear();
        qualify_win_info_ = WinInfo{};
        qualify_has_win_ = false;
        qualify_const_ = nullptr;
        qualify_op_ = -1;
        qualify_win_on_left_ = true;
        fallback_rows_.clear();
        sort_win_ = nullptr;
        sort_single_col_ok_ = false;
        sort_col_ = INVALID_INDEX;
        sort_asc_ = true;
        sort_tid_ = LogicalTypeId::SQLNULL;
        sort_i32_.clear();
        sort_i64_.clear();
        sort_d_.clear();
        sort_s_.clear();
        partition_sorted_.clear();
    }

    bool GetData(DataChunk &result) override {
        if (!setup_done_) { Setup(); setup_done_ = true; }

        if (use_fallback_) {
            if (emit_pos_ >= fallback_rows_.size()) return false;
            result.Initialize(GetTypes());
            idx_t count = 0;
            while (emit_pos_ < fallback_rows_.size() && count < VECTOR_SIZE) {
                auto &row = fallback_rows_[emit_pos_];
                for (idx_t col = 0; col < row.size(); col++) {
                    result.SetValue(col, count, row[col]);
                }
                emit_pos_++;
                count++;
            }
            result.SetCardinality(count);
            return count > 0;
        }

        if (emit_part_ >= partitions_.size()) return false;

        if (result.ColumnCount() != GetTypes().size()) result.Initialize(GetTypes());
        else result.Reset();

        idx_t out = 0;
        while (emit_part_ < partitions_.size() && out < VECTOR_SIZE) {
            // Lazy sort: only pay sort cost for partitions we actually emit from.
            if (emit_pos_ == 0) SortOnePartition(emit_part_);
            auto &idxs = partitions_[emit_part_];
            while (emit_pos_ < idxs.size() && out < VECTOR_SIZE) {
                idx_t input_row = idxs[emit_pos_];

                if (qualify_has_win_ && !EvalQualify(emit_part_, emit_pos_)) {
                    emit_pos_++;
                    continue;
                }

                for (idx_t col = 0; col < select_list_.size(); col++) {
                    auto &expr = select_list_[col];
                    auto et = expr->GetExpressionType();
                    Value v;
                    if (et == BoundExpressionType::WINDOW) {
                        v = ComputeWindowAt(select_win_info_[col], emit_part_, emit_pos_);
                    } else if (et == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*expr);
                        v = input_.Get(input_row, ref.column_index);
                    } else if (et == BoundExpressionType::CONSTANT) {
                        v = static_cast<BoundConstant &>(*expr).value;
                    } else {
                        // Generic expression: materialize row and evaluate.
                        DataChunk single;
                        auto in_types = children[0]->GetTypes();
                        single.Initialize(in_types);
                        for (idx_t c = 0; c < input_.types.size(); c++) {
                            single.SetValue(c, 0, input_.Get(input_row, c));
                        }
                        Vector res(expr->GetReturnType());
                        ExpressionExecutor::Execute(*expr, single, res, 1);
                        v = res.GetValue(0);
                    }
                    result.SetValue(col, out, v);
                }

                emit_pos_++;
                out++;
            }
            if (emit_pos_ >= idxs.size()) {
                emit_part_++;
                emit_pos_ = 0;
                rank_cur_ = 1;
                rank_prev_ = INVALID_INDEX;
                dense_cur_ = 1;
                dense_prev_ = INVALID_INDEX;
            }
        }

        result.SetCardinality(out);
        return out > 0;
    }

private:
    // Walk an expression tree, marking column indices that are referenced.
    static void CollectRefs(const BoundExpression &e, std::vector<bool> &used) {
        switch (e.GetExpressionType()) {
        case BoundExpressionType::COLUMN_REF: {
            auto &r = static_cast<const BoundColumnRef &>(e);
            if (r.column_index < used.size()) used[r.column_index] = true;
            break;
        }
        case BoundExpressionType::WINDOW: {
            auto &w = static_cast<const BoundWindowExpression &>(e);
            for (auto &p : w.partition_by) CollectRefs(*p, used);
            for (auto &o : w.order_by) CollectRefs(*o.expression, used);
            for (auto &a : w.arguments) CollectRefs(*a, used);
            break;
        }
        case BoundExpressionType::COMPARISON: {
            auto &c = static_cast<const BoundComparison &>(e);
            CollectRefs(*c.left, used);
            CollectRefs(*c.right, used);
            break;
        }
        case BoundExpressionType::CONJUNCTION: {
            auto &c = static_cast<const BoundConjunction &>(e);
            CollectRefs(*c.left, used);
            CollectRefs(*c.right, used);
            break;
        }
        case BoundExpressionType::NEGATION: {
            auto &n = static_cast<const BoundNegation &>(e);
            CollectRefs(*n.child, used);
            break;
        }
        case BoundExpressionType::IS_NULL: {
            auto &n = static_cast<const BoundIsNull &>(e);
            CollectRefs(*n.child, used);
            break;
        }
        case BoundExpressionType::ARITHMETIC: {
            auto &a = static_cast<const BoundArithmetic &>(e);
            CollectRefs(*a.left, used);
            CollectRefs(*a.right, used);
            break;
        }
        case BoundExpressionType::FUNCTION: {
            auto &f = static_cast<const BoundFunction &>(e);
            for (auto &a : f.arguments) CollectRefs(*a, used);
            break;
        }
        case BoundExpressionType::UNARY_MINUS: {
            auto &u = static_cast<const BoundUnaryMinus &>(e);
            CollectRefs(*u.child, used);
            break;
        }
        case BoundExpressionType::CAST: {
            auto &c = static_cast<const BoundCast &>(e);
            CollectRefs(*c.child, used);
            break;
        }
        case BoundExpressionType::CONSTANT:
        case BoundExpressionType::STAR:
            // No column refs.
            break;
        default:
            // SUBQUERY or unknown — be conservative: mark all needed.
            for (idx_t i = 0; i < used.size(); i++) used[i] = true;
            break;
        }
    }

    // Input buffer: stores chunks as-is + a global row → (chunk,pos) lookup.
    // This avoids the O(N*C) Value allocation that dominated the old path.
    struct InputBuf {
        std::vector<DataChunk> chunks;
        std::vector<uint32_t> chunk_of; // global row → chunk idx
        std::vector<uint32_t> pos_of;   // global row → row within chunk
        idx_t total = 0;
        std::vector<LogicalTypeId> types;

        idx_t size() const { return total; }

        Value Get(idx_t row, idx_t col) const {
            return chunks[chunk_of[row]].GetValue(col, pos_of[row]);
        }
        bool IsNull(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return !v.GetValidity().RowIsValid(pos_of[row]);
        }
        int32_t GetInt32(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const int32_t *>(v.GetData())[pos_of[row]];
        }
        int64_t GetInt64(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const int64_t *>(v.GetData())[pos_of[row]];
        }
        double GetDouble(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const double *>(v.GetData())[pos_of[row]];
        }
        const string_t &GetStr(idx_t row, idx_t col) const {
            auto &v = chunks[chunk_of[row]].GetVector(col);
            return reinterpret_cast<const string_t *>(v.GetData())[pos_of[row]];
        }
        // Read int64 from any integer column (INTEGER or BIGINT); 0 if other.
        int64_t ReadI64(idx_t row, idx_t col) const {
            auto tid = types[col];
            if (tid == LogicalTypeId::BIGINT) return GetInt64(row, col);
            if (tid == LogicalTypeId::INTEGER) return GetInt32(row, col);
            return 0;
        }
    };

    // Per-window metadata precomputed during Setup.
    enum WinFunc {
        WF_ROW_NUMBER, WF_RANK, WF_DENSE_RANK, WF_NTILE,
        WF_LAG, WF_LEAD, WF_FIRST_VALUE, WF_LAST_VALUE,
        WF_SUM, WF_COUNT, WF_AVG, WF_MIN, WF_MAX, WF_UNKNOWN
    };

    static WinFunc ResolveWinFunc(const std::string &name) {
        if (name == "ROW_NUMBER") return WF_ROW_NUMBER;
        if (name == "RANK") return WF_RANK;
        if (name == "DENSE_RANK") return WF_DENSE_RANK;
        if (name == "NTILE") return WF_NTILE;
        if (name == "LAG") return WF_LAG;
        if (name == "LEAD") return WF_LEAD;
        if (name == "FIRST_VALUE") return WF_FIRST_VALUE;
        if (name == "LAST_VALUE") return WF_LAST_VALUE;
        if (name == "SUM") return WF_SUM;
        if (name == "COUNT") return WF_COUNT;
        if (name == "AVG") return WF_AVG;
        if (name == "MIN") return WF_MIN;
        if (name == "MAX") return WF_MAX;
        return WF_UNKNOWN;
    }

    struct WinInfo {
        WinFunc fn = WF_UNKNOWN;
        idx_t arg_col = INVALID_INDEX;
        LogicalTypeId arg_tid = LogicalTypeId::SQLNULL;
        int64_t lag_offset = 1;
        int64_t ntile_buckets = 1;
        idx_t rank_order_col = INVALID_INDEX;
        LogicalTypeId rank_order_tid = LogicalTypeId::SQLNULL;
        // Precomputed Value per partition (only for aggregate-shape windows).
        std::vector<Value> part_value;
    };

    // Reads entire child input into InputBuf with typed raw Vector storage.
    void ReadInput() {
        idx_t num_cols = children[0]->GetTypes().size();
        input_.types.clear();
        input_.types.reserve(num_cols);
        for (auto &t : children[0]->GetTypes()) input_.types.push_back(t.id());
        input_.chunks.clear();
        input_.chunks.reserve(16);
        input_.total = 0;
        while (true) {
            DataChunk ch;
            ch.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(ch)) break;
            idx_t cnt = ch.size();
            if (cnt == 0) continue;
            input_.total += cnt;
            input_.chunks.push_back(std::move(ch));
        }
        if (input_.total == 0) return;
        input_.chunk_of.resize(input_.total);
        input_.pos_of.resize(input_.total);
        idx_t gi = 0;
        for (idx_t ci = 0; ci < input_.chunks.size(); ci++) {
            idx_t sz = input_.chunks[ci].size();
            for (idx_t ri = 0; ri < sz; ri++) {
                input_.chunk_of[gi] = static_cast<uint32_t>(ci);
                input_.pos_of[gi] = static_cast<uint32_t>(ri);
                gi++;
            }
        }
    }

    // True if two window expressions share the same PARTITION BY and ORDER BY
    // (by column reference and direction).
    static bool SamePartitionAndOrder(const BoundWindowExpression &a,
                                       const BoundWindowExpression &b) {
        if (a.partition_by.size() != b.partition_by.size()) return false;
        if (a.order_by.size() != b.order_by.size()) return false;
        for (idx_t i = 0; i < a.partition_by.size(); i++) {
            auto *ea = a.partition_by[i].get();
            auto *eb = b.partition_by[i].get();
            if (ea->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (eb->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (static_cast<const BoundColumnRef *>(ea)->column_index !=
                static_cast<const BoundColumnRef *>(eb)->column_index) return false;
        }
        for (idx_t i = 0; i < a.order_by.size(); i++) {
            auto *ea = a.order_by[i].expression.get();
            auto *eb = b.order_by[i].expression.get();
            if (ea->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (eb->GetExpressionType() != BoundExpressionType::COLUMN_REF) return false;
            if (static_cast<const BoundColumnRef *>(ea)->column_index !=
                static_cast<const BoundColumnRef *>(eb)->column_index) return false;
            if (a.order_by[i].ascending != b.order_by[i].ascending) return false;
        }
        return true;
    }

    // Build partitions_ using the reference window's PARTITION BY.
    void BuildPartitions(BoundWindowExpression &win) {
        idx_t n = input_.total;
        partitions_.clear();
        if (win.partition_by.empty()) {
            partitions_.emplace_back();
            partitions_[0].reserve(n);
            for (idx_t i = 0; i < n; i++) partitions_[0].push_back(i);
            return;
        }
        bool single_col = (win.partition_by.size() == 1 &&
            win.partition_by[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        LogicalTypeId tid = LogicalTypeId::SQLNULL;
        idx_t part_col = INVALID_INDEX;
        if (single_col) {
            auto &ref = static_cast<BoundColumnRef &>(*win.partition_by[0]);
            part_col = ref.column_index;
            if (part_col < input_.types.size()) tid = input_.types[part_col];
        }
        if (single_col && (tid == LogicalTypeId::INTEGER || tid == LogicalTypeId::BIGINT)) {
            std::unordered_map<int64_t, idx_t> k2p;
            k2p.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                int64_t k = input_.ReadI64(i, part_col);
                auto it = k2p.find(k);
                idx_t pi;
                if (it == k2p.end()) { pi = partitions_.size(); partitions_.emplace_back(); k2p.emplace(k, pi); }
                else pi = it->second;
                partitions_[pi].push_back(i);
            }
        } else if (single_col && tid == LogicalTypeId::VARCHAR) {
            // Linear-cache fast path — most real VARCHAR PARTITION BY is
            // low-cardinality (~10s of categories). Keep a small inline array
            // and do memcmp against it — much faster than std::unordered_map's
            // string hashing + allocation per row.
            struct Entry { const char *data; uint32_t len; idx_t pi; };
            std::vector<Entry> cache;
            cache.reserve(64);
            std::unordered_map<std::string, idx_t> overflow; // used if cardinality > cache cap
            const idx_t cache_cap = 256;
            for (idx_t i = 0; i < n; i++) {
                auto &s = input_.GetStr(i, part_col);
                const char *d = s.GetData();
                uint32_t l = s.GetSize();
                idx_t pi = INVALID_INDEX;
                if (cache.size() < cache_cap) {
                    for (auto &e : cache) {
                        if (e.len == l && memcmp(e.data, d, l) == 0) { pi = e.pi; break; }
                    }
                    if (pi == INVALID_INDEX) {
                        pi = partitions_.size();
                        partitions_.emplace_back();
                        cache.push_back({d, l, pi});
                    }
                } else {
                    std::string key(d, l);
                    auto it = overflow.find(key);
                    if (it == overflow.end()) {
                        pi = partitions_.size();
                        partitions_.emplace_back();
                        overflow.emplace(std::move(key), pi);
                    } else pi = it->second;
                }
                partitions_[pi].push_back(i);
            }
        } else {
            std::unordered_map<std::string, idx_t> k2p;
            k2p.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                std::string key;
                for (auto &p : win.partition_by) {
                    if (p->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*p);
                        auto v = input_.Get(i, ref.column_index);
                        if (v.IsNull()) key += "\x01N";
                        else key += v.ToString();
                        key += '|';
                    }
                }
                auto it = k2p.find(key);
                idx_t pi;
                if (it == k2p.end()) { pi = partitions_.size(); partitions_.emplace_back(); k2p.emplace(std::move(key), pi); }
                else pi = it->second;
                partitions_[pi].push_back(i);
            }
        }
    }

    // Extract sort-key data once at setup; actual sort happens lazily per
    // partition during emit (SortOnePartition). Critical for LIMIT queries
    // that emit from only a handful of partitions.
    void PrepareSortKeys(BoundWindowExpression &win) {
        sort_win_ = nullptr;
        sort_single_col_ok_ = false;
        if (win.order_by.empty()) return;
        sort_win_ = &win;
        bool single_ord = (win.order_by.size() == 1 &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        if (!single_ord) return;
        auto &ref = static_cast<BoundColumnRef &>(*win.order_by[0].expression);
        sort_col_ = ref.column_index;
        sort_asc_ = win.order_by[0].ascending;
        sort_tid_ = (sort_col_ < input_.types.size()) ? input_.types[sort_col_] : LogicalTypeId::SQLNULL;
        idx_t n = input_.total;
        if (sort_tid_ == LogicalTypeId::INTEGER) {
            sort_i32_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_i32_[i] = input_.GetInt32(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::BIGINT) {
            sort_i64_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_i64_[i] = input_.GetInt64(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::DOUBLE) {
            sort_d_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_d_[i] = input_.GetDouble(i, sort_col_);
            sort_single_col_ok_ = true;
        } else if (sort_tid_ == LogicalTypeId::VARCHAR) {
            sort_s_.resize(n);
            for (idx_t i = 0; i < n; i++) sort_s_[i] = input_.GetStr(i, sort_col_);
            sort_single_col_ok_ = true;
        }
    }

    void SortOnePartition(idx_t p) {
        if (p >= partition_sorted_.size() || partition_sorted_[p]) return;
        auto &idxs = partitions_[p];
        if (!sort_win_) { partition_sorted_[p] = true; return; }
        // Use partial_sort when LIMIT is small and we're sorting the first (or only)
        // partition — takes only the top-K rows in order, O(N log K) vs O(N log N).
        // Safe for ROW_NUMBER/NTILE/LAG/LEAD since consumer LIMIT stops the emit.
        idx_t partial_k = 0;
        if (row_limit_ > 0 && row_limit_ < idxs.size()) {
            // Only apply to the first partition — later partitions might contribute rows
            // if earlier ones don't fill the limit; conservatively sort later ones fully.
            if (p == 0) partial_k = row_limit_;
        }
        auto do_sort = [&](auto cmp) {
            if (partial_k > 0) {
                std::partial_sort(idxs.begin(), idxs.begin() + partial_k, idxs.end(), cmp);
            } else {
                std::sort(idxs.begin(), idxs.end(), cmp);
            }
        };
        if (sort_single_col_ok_) {
            if (sort_tid_ == LogicalTypeId::INTEGER) {
                bool asc = sort_asc_;
                auto *keys = sort_i32_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? (keys[a] < keys[b]) : (keys[a] > keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::BIGINT) {
                bool asc = sort_asc_;
                auto *keys = sort_i64_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? (keys[a] < keys[b]) : (keys[a] > keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::DOUBLE) {
                bool asc = sort_asc_;
                auto *keys = sort_d_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    return asc ? (keys[a] < keys[b]) : (keys[a] > keys[b]);
                });
            } else if (sort_tid_ == LogicalTypeId::VARCHAR) {
                bool asc = sort_asc_;
                auto *keys = sort_s_.data();
                do_sort([keys, asc](idx_t a, idx_t b) {
                    auto &sa = keys[a]; auto &sb = keys[b];
                    uint32_t la = sa.GetSize(), lb = sb.GetSize();
                    uint32_t m = la < lb ? la : lb;
                    int c = memcmp(sa.GetData(), sb.GetData(), m);
                    if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                    return asc ? (c < 0) : (c > 0);
                });
            }
        } else {
            auto &win = *sort_win_;
            std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                for (auto &ord : win.order_by) {
                    if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                        auto va = input_.Get(a, ref.column_index);
                        auto vb = input_.Get(b, ref.column_index);
                        if (va < vb) return ord.ascending;
                        if (vb < va) return !ord.ascending;
                    }
                }
                return false;
            });
        }
        partition_sorted_[p] = true;
    }

    // Resolve a window expression into WinInfo and precompute per-partition
    // values for aggregate-shape windows (SUM/COUNT/AVG/MIN/MAX/FIRST_VALUE/LAST_VALUE).
    void CompileWindow(BoundWindowExpression &win, WinInfo &info) {
        info.fn = ResolveWinFunc(win.function_name);
        info.arg_col = INVALID_INDEX;
        info.arg_tid = LogicalTypeId::SQLNULL;
        if (!win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            info.arg_col = static_cast<BoundColumnRef &>(*win.arguments[0]).column_index;
            if (info.arg_col < input_.types.size()) info.arg_tid = input_.types[info.arg_col];
        }
        info.lag_offset = 1;
        if (win.arguments.size() > 1 &&
            win.arguments[1]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[1]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) info.lag_offset = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) info.lag_offset = c.value.GetValue<int64_t>();
        }
        info.ntile_buckets = 1;
        if (info.fn == WF_NTILE && !win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[0]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) info.ntile_buckets = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) info.ntile_buckets = c.value.GetValue<int64_t>();
            if (info.ntile_buckets <= 0) info.ntile_buckets = 1;
        }
        info.rank_order_col = INVALID_INDEX;
        info.rank_order_tid = LogicalTypeId::SQLNULL;
        if (!win.order_by.empty() &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            info.rank_order_col = static_cast<BoundColumnRef &>(*win.order_by[0].expression).column_index;
            if (info.rank_order_col < input_.types.size())
                info.rank_order_tid = input_.types[info.rank_order_col];
        }

        // SUM/COUNT/AVG/MIN/MAX over whole partition don't require sorted order
        // (we treat ORDER BY as irrelevant for these — full-partition aggregate).
        // FIRST/LAST_VALUE are computed on-demand at emit time (they need sorted order).
        if (info.fn == WF_SUM || info.fn == WF_COUNT || info.fn == WF_AVG ||
            info.fn == WF_MIN || info.fn == WF_MAX) {
            info.part_value.resize(partitions_.size());
            for (idx_t p = 0; p < partitions_.size(); p++) {
                info.part_value[p] = ComputePartitionAggregate(info, partitions_[p]);
            }
        }
    }

    Value ComputePartitionAggregate(const WinInfo &info, const std::vector<idx_t> &indices) {
        idx_t ps = indices.size();
        if (ps == 0) return Value();
        if (info.fn == WF_FIRST_VALUE) {
            if (info.arg_col == INVALID_INDEX) return Value();
            return input_.Get(indices[0], info.arg_col);
        }
        if (info.fn == WF_LAST_VALUE) {
            if (info.arg_col == INVALID_INDEX) return Value();
            return input_.Get(indices[ps - 1], info.arg_col);
        }
        double sum = 0;
        int64_t count = 0;
        double min_d = 0, max_d = 0;
        bool has_mm = false;
        if (info.arg_col == INVALID_INDEX) {
            count = static_cast<int64_t>(ps);
        } else if (info.arg_tid == LogicalTypeId::INTEGER) {
            for (auto idx : indices) if (!input_.IsNull(idx, info.arg_col)) {
                double d = input_.GetInt32(idx, info.arg_col);
                count++; sum += d;
                if (!has_mm) { min_d = max_d = d; has_mm = true; }
                else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
            }
        } else if (info.arg_tid == LogicalTypeId::BIGINT) {
            for (auto idx : indices) if (!input_.IsNull(idx, info.arg_col)) {
                double d = static_cast<double>(input_.GetInt64(idx, info.arg_col));
                count++; sum += d;
                if (!has_mm) { min_d = max_d = d; has_mm = true; }
                else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
            }
        } else if (info.arg_tid == LogicalTypeId::DOUBLE) {
            for (auto idx : indices) if (!input_.IsNull(idx, info.arg_col)) {
                double d = input_.GetDouble(idx, info.arg_col);
                count++; sum += d;
                if (!has_mm) { min_d = max_d = d; has_mm = true; }
                else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
            }
        } else {
            Value min_v, max_v;
            for (auto idx : indices) {
                auto v = input_.Get(idx, info.arg_col);
                if (!v.IsNull()) {
                    count++;
                    auto tid = v.type().id();
                    double d = 0;
                    if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                    else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                    else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                    sum += d;
                    if (!has_mm || v < min_v) min_v = v;
                    if (!has_mm || v > max_v) max_v = v;
                    has_mm = true;
                }
            }
            if (info.fn == WF_COUNT) return Value::BIGINT(count);
            if (info.fn == WF_SUM) {
                if (info.arg_tid == LogicalTypeId::DOUBLE || info.arg_tid == LogicalTypeId::FLOAT)
                    return Value::DOUBLE(sum);
                return Value::BIGINT(static_cast<int64_t>(sum));
            }
            if (info.fn == WF_AVG) return count > 0 ? Value::DOUBLE(sum / count) : Value();
            if (info.fn == WF_MIN) return has_mm ? min_v : Value();
            if (info.fn == WF_MAX) return has_mm ? max_v : Value();
            return Value();
        }
        if (info.fn == WF_COUNT) return Value::BIGINT(count);
        if (info.fn == WF_SUM) {
            // Return type matches argument's numeric type (DOUBLE for DOUBLE, BIGINT otherwise).
            if (info.arg_tid == LogicalTypeId::DOUBLE) return Value::DOUBLE(sum);
            return Value::BIGINT(static_cast<int64_t>(sum));
        }
        if (info.fn == WF_AVG) return count > 0 ? Value::DOUBLE(sum / count) : Value();
        if (info.fn == WF_MIN) {
            if (!has_mm) return Value();
            if (info.arg_tid == LogicalTypeId::INTEGER) return Value::INTEGER(static_cast<int32_t>(min_d));
            if (info.arg_tid == LogicalTypeId::BIGINT) return Value::BIGINT(static_cast<int64_t>(min_d));
            return Value::DOUBLE(min_d);
        }
        if (info.fn == WF_MAX) {
            if (!has_mm) return Value();
            if (info.arg_tid == LogicalTypeId::INTEGER) return Value::INTEGER(static_cast<int32_t>(max_d));
            if (info.arg_tid == LogicalTypeId::BIGINT) return Value::BIGINT(static_cast<int64_t>(max_d));
            return Value::DOUBLE(max_d);
        }
        return Value();
    }

    // Compute the window value for one row during streaming emit.
    Value ComputeWindowAt(const WinInfo &info, idx_t part_idx, idx_t pos) {
        auto &indices = partitions_[part_idx];
        idx_t ps = indices.size();
        switch (info.fn) {
        case WF_ROW_NUMBER:
            return Value::BIGINT(static_cast<int64_t>(pos + 1));
        case WF_RANK: {
            if (pos == 0) { rank_cur_ = 1; rank_prev_ = indices[0]; return Value::BIGINT(1); }
            bool same = OrderEqual(info, rank_prev_, indices[pos]);
            if (!same) rank_cur_ = static_cast<int64_t>(pos + 1);
            rank_prev_ = indices[pos];
            return Value::BIGINT(rank_cur_);
        }
        case WF_DENSE_RANK: {
            if (pos == 0) { dense_cur_ = 1; dense_prev_ = indices[0]; return Value::BIGINT(1); }
            bool same = OrderEqual(info, dense_prev_, indices[pos]);
            if (!same) dense_cur_++;
            dense_prev_ = indices[pos];
            return Value::BIGINT(dense_cur_);
        }
        case WF_NTILE:
            return Value::BIGINT(static_cast<int64_t>(pos * info.ntile_buckets / ps) + 1);
        case WF_LAG: {
            int64_t target = static_cast<int64_t>(pos) - info.lag_offset;
            if (target >= 0 && target < static_cast<int64_t>(ps) && info.arg_col != INVALID_INDEX)
                return input_.Get(indices[static_cast<idx_t>(target)], info.arg_col);
            return Value();
        }
        case WF_LEAD: {
            int64_t target = static_cast<int64_t>(pos) + info.lag_offset;
            if (target >= 0 && target < static_cast<int64_t>(ps) && info.arg_col != INVALID_INDEX)
                return input_.Get(indices[static_cast<idx_t>(target)], info.arg_col);
            return Value();
        }
        case WF_FIRST_VALUE:
            if (info.arg_col == INVALID_INDEX || ps == 0) return Value();
            return input_.Get(indices[0], info.arg_col);
        case WF_LAST_VALUE:
            if (info.arg_col == INVALID_INDEX || ps == 0) return Value();
            return input_.Get(indices[ps - 1], info.arg_col);
        case WF_SUM:
        case WF_COUNT:
        case WF_AVG:
        case WF_MIN:
        case WF_MAX:
            return (part_idx < info.part_value.size()) ? info.part_value[part_idx] : Value();
        default:
            return Value();
        }
    }

    bool OrderEqual(const WinInfo &info, idx_t a, idx_t b) {
        if (info.rank_order_col == INVALID_INDEX) return false;
        switch (info.rank_order_tid) {
        case LogicalTypeId::INTEGER:
            return input_.GetInt32(a, info.rank_order_col) == input_.GetInt32(b, info.rank_order_col);
        case LogicalTypeId::BIGINT:
            return input_.GetInt64(a, info.rank_order_col) == input_.GetInt64(b, info.rank_order_col);
        case LogicalTypeId::DOUBLE:
            return input_.GetDouble(a, info.rank_order_col) == input_.GetDouble(b, info.rank_order_col);
        case LogicalTypeId::VARCHAR: {
            auto &sa = input_.GetStr(a, info.rank_order_col);
            auto &sb = input_.GetStr(b, info.rank_order_col);
            if (sa.GetSize() != sb.GetSize()) return false;
            return memcmp(sa.GetData(), sb.GetData(), sa.GetSize()) == 0;
        }
        default: {
            auto va = input_.Get(a, info.rank_order_col);
            auto vb = input_.Get(b, info.rank_order_col);
            return va == vb;
        }
        }
    }

    bool EvalQualify(idx_t part_idx, idx_t pos) {
        Value wv = ComputeWindowAt(qualify_win_info_, part_idx, pos);
        if (wv.IsNull() || !qualify_const_) return false;
        auto &cv = qualify_const_->value;
        auto ctid = cv.type().id();
        bool fast_int = (ctid == LogicalTypeId::INTEGER || ctid == LogicalTypeId::BIGINT);
        if (fast_int && wv.type().id() == LogicalTypeId::BIGINT) {
            int64_t cl = (ctid == LogicalTypeId::INTEGER) ? cv.GetValue<int32_t>() : cv.GetValue<int64_t>();
            int64_t wl = wv.GetValue<int64_t>();
            int64_t lhs = qualify_win_on_left_ ? wl : cl;
            int64_t rhs = qualify_win_on_left_ ? cl : wl;
            switch (qualify_op_) {
            case 0: return lhs == rhs;
            case 1: return lhs < rhs;
            case 2: return lhs <= rhs;
            case 3: return lhs > rhs;
            case 4: return lhs >= rhs;
            case 5: return lhs != rhs;
            default: return false;
            }
        }
        const Value &lhs = qualify_win_on_left_ ? wv : cv;
        const Value &rhs = qualify_win_on_left_ ? cv : wv;
        switch (qualify_op_) {
        case 0: return !(lhs < rhs) && !(rhs < lhs);
        case 1: return lhs < rhs;
        case 2: return lhs <= rhs;
        case 3: return lhs > rhs;
        case 4: return lhs >= rhs;
        case 5: return lhs != rhs;
        default: return false;
        }
    }

    void Setup() {
        // Column pruning: push projection mask down to file scan so we skip
        // parsing columns not referenced by SELECT or QUALIFY.
        idx_t num_cols = children[0]->GetTypes().size();
        std::vector<bool> needed(num_cols, false);
        for (auto &s : select_list_) CollectRefs(*s, needed);
        if (qualify_) CollectRefs(*qualify_, needed);
        bool any = false;
        for (bool b : needed) if (b) { any = true; break; }
        if (any) {
            children[0]->SetNeededOutputs(needed);
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                fs->SetProjection(needed);
            }
        }
        ReadInput();
        if (input_.total == 0) return;

        // Collect all window expressions (from SELECT and QUALIFY).
        std::vector<BoundWindowExpression *> wins;
        for (auto &s : select_list_) {
            if (s->GetExpressionType() == BoundExpressionType::WINDOW)
                wins.push_back(static_cast<BoundWindowExpression *>(s.get()));
        }
        BoundWindowExpression *qwin = nullptr;
        if (qualify_ && qualify_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*qualify_);
            if (cmp.left->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.left.get());
                qualify_win_on_left_ = true;
                if (cmp.right->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qualify_const_ = static_cast<BoundConstant *>(cmp.right.get());
            } else if (cmp.right->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.right.get());
                qualify_win_on_left_ = false;
                if (cmp.left->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qualify_const_ = static_cast<BoundConstant *>(cmp.left.get());
            }
            if (qwin) {
                wins.push_back(qwin);
                const std::string &op = cmp.op;
                if (op == "=") qualify_op_ = 0;
                else if (op == "<") qualify_op_ = 1;
                else if (op == "<=") qualify_op_ = 2;
                else if (op == ">") qualify_op_ = 3;
                else if (op == ">=") qualify_op_ = 4;
                else if (op == "!=" || op == "<>") qualify_op_ = 5;
                else qualify_op_ = -1;
                qualify_has_win_ = true;
            }
        }

        if (wins.empty()) {
            // No windows at all — trivial case. Single partition, no sort.
            partitions_.emplace_back();
            partitions_[0].reserve(input_.total);
            for (idx_t i = 0; i < input_.total; i++) partitions_[0].push_back(i);
            select_win_info_.resize(select_list_.size());
            return;
        }

        // All windows must share PARTITION BY + ORDER BY. If not, fall back
        // to the generic per-column compute (builds full output once).
        auto *ref_win = wins.front();
        for (idx_t i = 1; i < wins.size(); i++) {
            if (!SamePartitionAndOrder(*ref_win, *wins[i])) {
                use_fallback_ = true;
                break;
            }
        }

        if (use_fallback_) {
            BuildFallbackRows();
            return;
        }

        BuildPartitions(*ref_win);
        partition_sorted_.assign(partitions_.size(), false);
        PrepareSortKeys(*ref_win);

        select_win_info_.resize(select_list_.size());
        for (idx_t col = 0; col < select_list_.size(); col++) {
            if (select_list_[col]->GetExpressionType() == BoundExpressionType::WINDOW) {
                auto &w = static_cast<BoundWindowExpression &>(*select_list_[col]);
                CompileWindow(w, select_win_info_[col]);
            }
        }
        if (qwin) CompileWindow(*qwin, qualify_win_info_);

        // Fast path: QUALIFY ROW_NUMBER() OVER (... ORDER BY X) = 1.
        // Just reduce each partition to its argmin/argmax row — no sort.
        TryTop1Qualify(ref_win, qwin);
    }

    void TryTop1Qualify(BoundWindowExpression *ref_win, BoundWindowExpression *qwin) {
        if (!qwin) return;
        if (qualify_win_info_.fn != WF_ROW_NUMBER) return;
        if (qualify_op_ != 0) return;           // must be =
        if (!qualify_const_) return;
        if (!qualify_win_on_left_) return;      // only rn = K form
        auto ctid = qualify_const_->value.type().id();
        int64_t k;
        if (ctid == LogicalTypeId::INTEGER) k = qualify_const_->value.GetValue<int32_t>();
        else if (ctid == LogicalTypeId::BIGINT) k = qualify_const_->value.GetValue<int64_t>();
        else return;
        if (k != 1) return;
        if (ref_win->order_by.empty()) return;
        if (ref_win->order_by[0].expression->GetExpressionType() != BoundExpressionType::COLUMN_REF) return;

        auto &ord_ref = static_cast<BoundColumnRef &>(*ref_win->order_by[0].expression);
        idx_t col = ord_ref.column_index;
        LogicalTypeId tid = (col < input_.types.size()) ? input_.types[col] : LogicalTypeId::SQLNULL;
        bool asc = ref_win->order_by[0].ascending;

        // Reduce each partition to a single winner row — the argmin (asc) or argmax (desc).
        auto pick_one = [&](std::vector<idx_t> &idxs) {
            if (idxs.empty()) return;
            idx_t best = idxs[0];
            if (tid == LogicalTypeId::INTEGER) {
                int32_t bk = input_.GetInt32(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    int32_t v = input_.GetInt32(idxs[j], col);
                    if ((asc && v < bk) || (!asc && v > bk)) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::BIGINT) {
                int64_t bk = input_.GetInt64(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    int64_t v = input_.GetInt64(idxs[j], col);
                    if ((asc && v < bk) || (!asc && v > bk)) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::DOUBLE) {
                double bk = input_.GetDouble(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    double v = input_.GetDouble(idxs[j], col);
                    if ((asc && v < bk) || (!asc && v > bk)) { bk = v; best = idxs[j]; }
                }
            } else if (tid == LogicalTypeId::VARCHAR) {
                auto bs = input_.GetStr(best, col);
                for (idx_t j = 1; j < idxs.size(); j++) {
                    auto v = input_.GetStr(idxs[j], col);
                    uint32_t la = v.GetSize(), lb = bs.GetSize();
                    uint32_t m = la < lb ? la : lb;
                    int c = memcmp(v.GetData(), bs.GetData(), m);
                    if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                    if ((asc && c < 0) || (!asc && c > 0)) { bs = v; best = idxs[j]; }
                }
            } else {
                return;
            }
            idxs.clear();
            idxs.push_back(best);
        };
        for (auto &idxs : partitions_) pick_one(idxs);
        // Each partition now has size 1 — subsequent lazy sort is a no-op,
        // and the emit loop only produces one row per partition. QUALIFY ROW_NUMBER=1
        // evaluates true at pos 0, so all rows pass.
        for (idx_t p = 0; p < partition_sorted_.size(); p++) partition_sorted_[p] = true;
    }

    // Fallback path: multi-window query with different partition/order.
    // Build fallback_rows_ using per-column compute (old logic).
    void BuildFallbackRows() {
        idx_t n = input_.total;
        idx_t num_output_cols = select_list_.size();
        std::vector<std::vector<Value>> output(n);
        for (auto &row : output) row.resize(num_output_cols);
        for (idx_t col = 0; col < num_output_cols; col++) {
            auto &expr = select_list_[col];
            auto et = expr->GetExpressionType();
            if (et == BoundExpressionType::WINDOW) {
                auto &win = static_cast<BoundWindowExpression &>(*expr);
                ComputeWindowColumn(win, input_, output, col);
            } else if (et == BoundExpressionType::COLUMN_REF) {
                auto &ref = static_cast<BoundColumnRef &>(*expr);
                for (idx_t i = 0; i < n; i++) output[i][col] = input_.Get(i, ref.column_index);
            } else if (et == BoundExpressionType::CONSTANT) {
                auto &c = static_cast<BoundConstant &>(*expr);
                for (idx_t i = 0; i < n; i++) output[i][col] = c.value;
            } else {
                auto in_types = children[0]->GetTypes();
                for (idx_t i = 0; i < n; i++) {
                    DataChunk single;
                    single.Initialize(in_types);
                    for (idx_t c = 0; c < input_.types.size(); c++)
                        single.SetValue(c, 0, input_.Get(i, c));
                    Vector res(expr->GetReturnType());
                    ExpressionExecutor::Execute(*expr, single, res, 1);
                    output[i][col] = res.GetValue(0);
                }
            }
        }
        if (qualify_ && qualify_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*qualify_);
            BoundWindowExpression *qwin = nullptr;
            BoundConstant *qcon = nullptr;
            bool win_left = true;
            if (cmp.left->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.left.get());
                if (cmp.right->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qcon = static_cast<BoundConstant *>(cmp.right.get());
            } else if (cmp.right->GetExpressionType() == BoundExpressionType::WINDOW) {
                qwin = static_cast<BoundWindowExpression *>(cmp.right.get());
                win_left = false;
                if (cmp.left->GetExpressionType() == BoundExpressionType::CONSTANT)
                    qcon = static_cast<BoundConstant *>(cmp.left.get());
            }
            if (qwin) {
                idx_t qual_col = num_output_cols;
                for (auto &row : output) row.push_back(Value());
                ComputeWindowColumn(*qwin, input_, output, qual_col);
                const std::string &op = cmp.op;
                for (idx_t i = 0; i < n; i++) {
                    auto &wv = output[i][qual_col];
                    if (wv.IsNull() || !qcon) continue;
                    auto &cv = qcon->value;
                    const Value &lhs = win_left ? wv : cv;
                    const Value &rhs = win_left ? cv : wv;
                    bool pass = false;
                    if (op == "=") pass = !(lhs < rhs) && !(rhs < lhs);
                    else if (op == "<") pass = lhs < rhs;
                    else if (op == "<=") pass = lhs <= rhs;
                    else if (op == ">") pass = lhs > rhs;
                    else if (op == ">=") pass = lhs >= rhs;
                    else if (op == "!=" || op == "<>") pass = lhs != rhs;
                    if (pass) {
                        auto row = std::move(output[i]);
                        row.resize(num_output_cols);
                        fallback_rows_.push_back(std::move(row));
                    }
                }
                return;
            }
        }
        fallback_rows_ = std::move(output);
    }

    void ComputeWindowColumn(BoundWindowExpression &win,
                              const InputBuf &input,
                              std::vector<std::vector<Value>> &output,
                              idx_t out_col) {
        idx_t n = input.size();
        if (n == 0) return;
        WinFunc fn = ResolveWinFunc(win.function_name);

        // Pre-resolve arg / ORDER BY column indices once.
        idx_t arg_col = INVALID_INDEX;
        if (!win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            arg_col = static_cast<BoundColumnRef &>(*win.arguments[0]).column_index;
        }
        int64_t lag_offset = 1;
        if (win.arguments.size() > 1 &&
            win.arguments[1]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[1]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) lag_offset = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) lag_offset = c.value.GetValue<int64_t>();
        }
        int64_t ntile_buckets = 1;
        if (fn == WF_NTILE && !win.arguments.empty() &&
            win.arguments[0]->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &c = static_cast<BoundConstant &>(*win.arguments[0]);
            auto ctid = c.value.type().id();
            if (ctid == LogicalTypeId::INTEGER) ntile_buckets = c.value.GetValue<int32_t>();
            else if (ctid == LogicalTypeId::BIGINT) ntile_buckets = c.value.GetValue<int64_t>();
            if (ntile_buckets <= 0) ntile_buckets = 1;
        }
        idx_t rank_order_col = INVALID_INDEX;
        LogicalTypeId rank_order_tid = LogicalTypeId::SQLNULL;
        if (!win.order_by.empty() &&
            win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
            rank_order_col = static_cast<BoundColumnRef &>(*win.order_by[0].expression).column_index;
            if (rank_order_col < input.types.size()) rank_order_tid = input.types[rank_order_col];
        }

        // Build partition groups. Typed fast paths for single-column int/varchar PARTITION BY.
        std::vector<std::vector<idx_t>> partitions_list;

        LogicalTypeId part_tid = LogicalTypeId::SQLNULL;
        idx_t part_col = INVALID_INDEX;
        bool single_col_partition = (win.partition_by.size() == 1 &&
            win.partition_by[0]->GetExpressionType() == BoundExpressionType::COLUMN_REF);
        if (single_col_partition) {
            auto &ref = static_cast<BoundColumnRef &>(*win.partition_by[0]);
            part_col = ref.column_index;
            if (part_col < input.types.size()) part_tid = input.types[part_col];
        }

        if (win.partition_by.empty()) {
            partitions_list.emplace_back();
            partitions_list[0].reserve(n);
            for (idx_t i = 0; i < n; i++) partitions_list[0].push_back(i);
        } else if (single_col_partition &&
                   (part_tid == LogicalTypeId::INTEGER || part_tid == LogicalTypeId::BIGINT)) {
            // Raw int64 key — no Value alloc, no hash on strings.
            std::unordered_map<int64_t, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                int64_t k = input.ReadI64(i, part_col);
                auto it = key_to_part.find(k);
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(k, p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        } else if (single_col_partition && part_tid == LogicalTypeId::VARCHAR) {
            // Raw string_t read — no Value/ToString alloc.
            std::unordered_map<std::string, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                auto &s = input.GetStr(i, part_col);
                auto it = key_to_part.find(std::string(s.GetData(), s.GetSize()));
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(std::string(s.GetData(), s.GetSize()), p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        } else {
            // Generic: Value::ToString() keyed map (slow but handles multi-col / exotic types).
            std::unordered_map<std::string, idx_t> key_to_part;
            key_to_part.reserve(n / 4 + 16);
            for (idx_t i = 0; i < n; i++) {
                std::string key;
                for (auto &p : win.partition_by) {
                    if (p->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &ref = static_cast<BoundColumnRef &>(*p);
                        auto v = input.Get(i, ref.column_index);
                        if (v.IsNull()) key += "\x01N";
                        else key += v.ToString();
                        key += '|';
                    }
                }
                auto it = key_to_part.find(key);
                idx_t p_idx;
                if (it == key_to_part.end()) {
                    p_idx = partitions_list.size();
                    partitions_list.emplace_back();
                    key_to_part.emplace(std::move(key), p_idx);
                } else {
                    p_idx = it->second;
                }
                partitions_list[p_idx].push_back(i);
            }
        }

        // Typed sort comparator for single-column ORDER BY (common case).
        // Falls back to Value comparisons for multi-col / exotic types.
        auto sort_partitions = [&]() {
            if (win.order_by.empty()) return;
            bool single_ord = (win.order_by.size() == 1 &&
                win.order_by[0].expression->GetExpressionType() == BoundExpressionType::COLUMN_REF);
            if (single_ord) {
                auto &ref = static_cast<BoundColumnRef &>(*win.order_by[0].expression);
                idx_t col = ref.column_index;
                bool asc = win.order_by[0].ascending;
                auto tid = (col < input.types.size()) ? input.types[col] : LogicalTypeId::SQLNULL;
                if (tid == LogicalTypeId::INTEGER) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            auto va = input.GetInt32(a, col);
                            auto vb = input.GetInt32(b, col);
                            return asc ? (va < vb) : (va > vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::BIGINT) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            auto va = input.GetInt64(a, col);
                            auto vb = input.GetInt64(b, col);
                            return asc ? (va < vb) : (va > vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::DOUBLE) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            auto va = input.GetDouble(a, col);
                            auto vb = input.GetDouble(b, col);
                            return asc ? (va < vb) : (va > vb);
                        });
                    }
                    return;
                }
                if (tid == LogicalTypeId::VARCHAR) {
                    for (auto &idxs : partitions_list) {
                        std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                            auto &sa = input.GetStr(a, col);
                            auto &sb = input.GetStr(b, col);
                            uint32_t la = sa.GetSize(), lb = sb.GetSize();
                            uint32_t m = la < lb ? la : lb;
                            int c = memcmp(sa.GetData(), sb.GetData(), m);
                            if (c == 0) c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
                            return asc ? (c < 0) : (c > 0);
                        });
                    }
                    return;
                }
            }
            // Generic Value-based fallback.
            for (auto &idxs : partitions_list) {
                std::sort(idxs.begin(), idxs.end(), [&](idx_t a, idx_t b) {
                    for (auto &ord : win.order_by) {
                        if (ord.expression->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                            auto &ref = static_cast<BoundColumnRef &>(*ord.expression);
                            auto va = input.Get(a, ref.column_index);
                            auto vb = input.Get(b, ref.column_index);
                            if (va < vb) return ord.ascending;
                            if (vb < va) return !ord.ascending;
                        }
                    }
                    return false;
                });
            }
        };
        sort_partitions();

        // Typed equality for RANK/DENSE_RANK tie detection (reads raw data).
        auto order_equal = [&](idx_t a, idx_t b) -> bool {
            if (rank_order_col == INVALID_INDEX) return false;
            switch (rank_order_tid) {
            case LogicalTypeId::INTEGER:
                return input.GetInt32(a, rank_order_col) == input.GetInt32(b, rank_order_col);
            case LogicalTypeId::BIGINT:
                return input.GetInt64(a, rank_order_col) == input.GetInt64(b, rank_order_col);
            case LogicalTypeId::DOUBLE:
                return input.GetDouble(a, rank_order_col) == input.GetDouble(b, rank_order_col);
            case LogicalTypeId::VARCHAR: {
                auto &sa = input.GetStr(a, rank_order_col);
                auto &sb = input.GetStr(b, rank_order_col);
                if (sa.GetSize() != sb.GetSize()) return false;
                return memcmp(sa.GetData(), sb.GetData(), sa.GetSize()) == 0;
            }
            default: {
                auto va = input.Get(a, rank_order_col);
                auto vb = input.Get(b, rank_order_col);
                return va == vb;
            }
            }
        };

        // Per-function O(n) computation. Function kind resolved once, outside the row loop.
        for (auto &indices : partitions_list) {
            idx_t ps = indices.size();
            if (ps == 0) continue;

            switch (fn) {
            case WF_ROW_NUMBER:
                for (idx_t pos = 0; pos < ps; pos++)
                    output[indices[pos]][out_col] = Value::BIGINT(static_cast<int64_t>(pos + 1));
                break;

            case WF_RANK: {
                int64_t cur_rank = 1;
                output[indices[0]][out_col] = Value::BIGINT(1);
                for (idx_t pos = 1; pos < ps; pos++) {
                    if (!order_equal(indices[pos], indices[pos - 1]))
                        cur_rank = static_cast<int64_t>(pos + 1);
                    output[indices[pos]][out_col] = Value::BIGINT(cur_rank);
                }
                break;
            }

            case WF_DENSE_RANK: {
                int64_t cur_rank = 1;
                output[indices[0]][out_col] = Value::BIGINT(1);
                for (idx_t pos = 1; pos < ps; pos++) {
                    if (!order_equal(indices[pos], indices[pos - 1])) cur_rank++;
                    output[indices[pos]][out_col] = Value::BIGINT(cur_rank);
                }
                break;
            }

            case WF_NTILE:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t bucket = static_cast<int64_t>(pos * ntile_buckets / ps) + 1;
                    output[indices[pos]][out_col] = Value::BIGINT(bucket);
                }
                break;

            case WF_LAG:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t target = static_cast<int64_t>(pos) - lag_offset;
                    if (target >= 0 && target < static_cast<int64_t>(ps) && arg_col != INVALID_INDEX) {
                        output[indices[pos]][out_col] =
                            input.Get(indices[static_cast<idx_t>(target)], arg_col);
                    } // else stays default NULL Value
                }
                break;

            case WF_LEAD:
                for (idx_t pos = 0; pos < ps; pos++) {
                    int64_t target = static_cast<int64_t>(pos) + lag_offset;
                    if (target >= 0 && target < static_cast<int64_t>(ps) && arg_col != INVALID_INDEX) {
                        output[indices[pos]][out_col] =
                            input.Get(indices[static_cast<idx_t>(target)], arg_col);
                    }
                }
                break;

            case WF_FIRST_VALUE: {
                Value v;
                if (arg_col != INVALID_INDEX) v = input.Get(indices[0], arg_col);
                for (idx_t pos = 0; pos < ps; pos++) output[indices[pos]][out_col] = v;
                break;
            }

            case WF_LAST_VALUE: {
                Value v;
                if (arg_col != INVALID_INDEX) v = input.Get(indices[ps - 1], arg_col);
                for (idx_t pos = 0; pos < ps; pos++) output[indices[pos]][out_col] = v;
                break;
            }

            case WF_SUM:
            case WF_COUNT:
            case WF_AVG:
            case WF_MIN:
            case WF_MAX: {
                // Aggregate over whole partition (typed fast path), broadcast to rows.
                double sum = 0;
                int64_t count = 0;
                double min_d = 0, max_d = 0;
                bool has_mm = false;
                bool is_str = false;
                bool has_str = false;
                string_t min_s, max_s;
                LogicalTypeId arg_tid = (arg_col != INVALID_INDEX && arg_col < input.types.size())
                                            ? input.types[arg_col] : LogicalTypeId::SQLNULL;
                if (arg_tid == LogicalTypeId::VARCHAR) is_str = true;

                if (arg_col == INVALID_INDEX) {
                    count = static_cast<int64_t>(ps);
                } else if (arg_tid == LogicalTypeId::INTEGER) {
                    for (auto idx : indices) {
                        if (!input.IsNull(idx, arg_col)) {
                            double d = input.GetInt32(idx, arg_col);
                            count++; sum += d;
                            if (!has_mm) { min_d = max_d = d; has_mm = true; }
                            else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
                        }
                    }
                } else if (arg_tid == LogicalTypeId::BIGINT) {
                    for (auto idx : indices) {
                        if (!input.IsNull(idx, arg_col)) {
                            double d = static_cast<double>(input.GetInt64(idx, arg_col));
                            count++; sum += d;
                            if (!has_mm) { min_d = max_d = d; has_mm = true; }
                            else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
                        }
                    }
                } else if (arg_tid == LogicalTypeId::DOUBLE) {
                    for (auto idx : indices) {
                        if (!input.IsNull(idx, arg_col)) {
                            double d = input.GetDouble(idx, arg_col);
                            count++; sum += d;
                            if (!has_mm) { min_d = max_d = d; has_mm = true; }
                            else { if (d < min_d) min_d = d; if (d > max_d) max_d = d; }
                        }
                    }
                } else if (is_str) {
                    // MIN/MAX only for strings; SUM/AVG undefined.
                    for (auto idx : indices) {
                        if (!input.IsNull(idx, arg_col)) {
                            auto &s = input.GetStr(idx, arg_col);
                            count++;
                            if (!has_str) { min_s = max_s = s; has_str = true; }
                            else {
                                uint32_t l_min = s.GetSize() < min_s.GetSize() ? s.GetSize() : min_s.GetSize();
                                int c_min = memcmp(s.GetData(), min_s.GetData(), l_min);
                                if (c_min < 0 || (c_min == 0 && s.GetSize() < min_s.GetSize())) min_s = s;
                                uint32_t l_max = s.GetSize() < max_s.GetSize() ? s.GetSize() : max_s.GetSize();
                                int c_max = memcmp(s.GetData(), max_s.GetData(), l_max);
                                if (c_max > 0 || (c_max == 0 && s.GetSize() > max_s.GetSize())) max_s = s;
                            }
                        }
                    }
                } else {
                    // Fallback via Value (rare types).
                    Value min_v, max_v;
                    for (auto idx : indices) {
                        auto v = input.Get(idx, arg_col);
                        if (!v.IsNull()) {
                            count++;
                            auto tid = v.type().id();
                            double d = 0;
                            if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                            else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                            else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                            sum += d;
                            if (!has_mm || v < min_v) min_v = v;
                            if (!has_mm || v > max_v) max_v = v;
                            has_mm = true;
                        }
                    }
                    Value result;
                    if (fn == WF_COUNT) result = Value::BIGINT(count);
                    else if (fn == WF_SUM) result = Value::BIGINT(static_cast<int64_t>(sum));
                    else if (fn == WF_AVG) result = count > 0 ? Value::DOUBLE(sum / count) : Value();
                    else if (fn == WF_MIN) result = has_mm ? min_v : Value();
                    else if (fn == WF_MAX) result = has_mm ? max_v : Value();
                    for (auto idx : indices) output[idx][out_col] = result;
                    break;
                }

                // Build result from typed accumulators.
                Value result;
                if (fn == WF_COUNT) {
                    result = Value::BIGINT(count);
                } else if (is_str) {
                    if (fn == WF_MIN) result = has_str ? Value::VARCHAR(std::string(min_s.GetData(), min_s.GetSize())) : Value();
                    else if (fn == WF_MAX) result = has_str ? Value::VARCHAR(std::string(max_s.GetData(), max_s.GetSize())) : Value();
                    else result = Value();  // SUM/AVG on string not defined
                } else {
                    if (fn == WF_SUM) result = Value::BIGINT(static_cast<int64_t>(sum));
                    else if (fn == WF_AVG) result = count > 0 ? Value::DOUBLE(sum / count) : Value();
                    else if (fn == WF_MIN) {
                        if (!has_mm) result = Value();
                        else if (arg_tid == LogicalTypeId::INTEGER) result = Value::INTEGER(static_cast<int32_t>(min_d));
                        else if (arg_tid == LogicalTypeId::BIGINT) result = Value::BIGINT(static_cast<int64_t>(min_d));
                        else result = Value::DOUBLE(min_d);
                    }
                    else if (fn == WF_MAX) {
                        if (!has_mm) result = Value();
                        else if (arg_tid == LogicalTypeId::INTEGER) result = Value::INTEGER(static_cast<int32_t>(max_d));
                        else if (arg_tid == LogicalTypeId::BIGINT) result = Value::BIGINT(static_cast<int64_t>(max_d));
                        else result = Value::DOUBLE(max_d);
                    }
                }
                for (auto idx : indices) output[idx][out_col] = result;
                break;
            }

            case WF_UNKNOWN:
                break;
            }
        }
    }

    std::vector<BoundExprPtr> select_list_;
    BoundExprPtr qualify_;

    // Streaming state.
    InputBuf input_;
    std::vector<std::vector<idx_t>> partitions_;
    std::vector<WinInfo> select_win_info_;
    WinInfo qualify_win_info_;
    bool qualify_has_win_ = false;
    BoundConstant *qualify_const_ = nullptr;
    int qualify_op_ = -1;             // 0=EQ 1=LT 2=LE 3=GT 4=GE 5=NE
    bool qualify_win_on_left_ = true;
    idx_t emit_part_ = 0;
    idx_t emit_pos_ = 0;
    bool setup_done_ = false;
    // Lazy-sort state — sort each partition on-demand at emit time.
    BoundWindowExpression *sort_win_ = nullptr;
    bool sort_single_col_ok_ = false;
    idx_t sort_col_ = INVALID_INDEX;
    bool sort_asc_ = true;
    LogicalTypeId sort_tid_ = LogicalTypeId::SQLNULL;
    std::vector<int32_t> sort_i32_;
    std::vector<int64_t> sort_i64_;
    std::vector<double> sort_d_;
    std::vector<string_t> sort_s_;
    std::vector<bool> partition_sorted_;
    // Streaming RANK/DENSE_RANK state, reset per partition.
    int64_t rank_cur_ = 1;
    idx_t rank_prev_ = INVALID_INDEX;
    int64_t dense_cur_ = 1;
    idx_t dense_prev_ = INVALID_INDEX;
    // Fallback path (multi-window, different partition/order).
    bool use_fallback_ = false;
    std::vector<std::vector<Value>> fallback_rows_;
    // LIMIT pushdown — 0 means unlimited (full sort).
    idx_t row_limit_ = 0;
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

    struct AggInfo {
        std::string name;
        idx_t col_idx;        // column index if simple column ref, else INVALID_INDEX
        bool is_count_star;
        bool is_distinct;
    };

    // Fused JOIN+aggregate hot-loop — defined out-of-class (needs full PhysicalHashJoin).
    // Returns true if it handled the computation (result_rows_ populated).
    bool TryComputeFusedJoinAggregate(PhysicalHashJoin *hj,
                                       const std::vector<AggInfo> &agg_infos,
                                       const std::vector<idx_t> &group_col_indices);

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

        // === FAST PATH: fused JOIN + GROUP BY ===
        if (auto *hj = AsHashJoin(children[0].get())) {
            if (TryComputeFusedJoinAggregate(hj, agg_infos, group_col_indices)) return;
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
        // Sequential path is single-col only; parallel path handles multi-col too.
        bool single_group_fast = (group_col_indices.size() == 1 && all_simple_aggs == false &&
                                  num_aggs > 0);
        bool multi_group_fast = (group_col_indices.size() > 1 && all_simple_aggs == false &&
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

        // Parallel single-column GROUP BY: split file into N slices,
        // each thread runs full parse+aggregate, then merge.
        PhysicalFileScan *file_scan_for_group = nullptr;
        if (single_group_fast) {
            file_scan_for_group = dynamic_cast<PhysicalFileScan *>(children[0].get());
        }
        // Parallel GROUP BY (single or multi col): activate for large files.
        PhysicalFileScan *parallel_scan = nullptr;
        // file_scan_for_group is only set when single_group_fast. Extend for multi-col too.
        if (!file_scan_for_group && multi_group_fast) {
            file_scan_for_group = dynamic_cast<PhysicalFileScan *>(children[0].get());
        }
        if ((single_group_fast || multi_group_fast) && file_scan_for_group &&
            file_scan_for_group->GetReader() &&
            file_scan_for_group->GetReader()->GetSize() > 16 * 1024 * 1024) {
            parallel_scan = file_scan_for_group;
            // Push projection down so threads skip unneeded columns.
            {
                idx_t nc = children[0]->GetTypes().size();
                std::vector<bool> need(nc, false);
                for (idx_t gc : group_col_indices) if (gc < nc) need[gc] = true;
                for (auto &info : agg_infos) if (info.col_idx != INVALID_INDEX && info.col_idx < nc) need[info.col_idx] = true;
                file_scan_for_group->SetProjection(need);
            }
            children[0]->Init(); // open reader + header if not already
        }
        if (parallel_scan) {
            auto *reader = parallel_scan->GetReader();
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

            bool multi_group = group_col_indices.size() > 1;
            idx_t group_col = group_col_indices[0];
            auto types = children[0]->GetTypes();

            struct ThreadState {
                std::unordered_map<int64_t, std::vector<AggState>> int_groups;
                std::unordered_map<std::string, std::vector<AggState>> str_groups;
                std::unordered_map<int64_t, Value> int_keys;
                std::unordered_map<std::string, std::vector<Value>> str_keys_multi;
                std::unordered_map<std::string, Value> str_keys;
            };
            std::vector<ThreadState> tstates(num_threads);

            auto projection = file_scan_for_group->GetProjection();

            std::vector<std::thread> threads;
            for (unsigned int t = 0; t < num_threads; t++) {
                threads.emplace_back([&, t]() {
                    auto &ts = tstates[t];
                    FastCSVReader thread_reader(buffer, ranges[t], ranges[t + 1], delim);
                    DataChunk chunk;
                    chunk.Initialize(types);

                    while (true) {
                        chunk.Reset();
                        idx_t cnt = projection.empty()
                            ? thread_reader.ReadChunk(chunk, types)
                            : thread_reader.ReadChunkProjected(chunk, types, projection);
                        if (cnt == 0) break;

                        auto &gvec = chunk.GetVector(group_col);
                        auto gtid = gvec.GetType().id();
                        bool is_int = (gtid == LogicalTypeId::INTEGER || gtid == LogicalTypeId::BIGINT) && !multi_group;

                        for (idx_t i = 0; i < cnt; i++) {
                            std::vector<AggState> *states_ptr = nullptr;
                            if (multi_group) {
                                // Concatenated string key for multi-col GROUP BY.
                                std::string k;
                                for (idx_t gci : group_col_indices) {
                                    auto &v = chunk.GetVector(gci);
                                    auto tid = v.GetType().id();
                                    if (!v.GetValidity().RowIsValid(i)) { k += "\x01N"; }
                                    else if (tid == LogicalTypeId::INTEGER)
                                        k += std::to_string(reinterpret_cast<const int32_t *>(v.GetData())[i]);
                                    else if (tid == LogicalTypeId::BIGINT)
                                        k += std::to_string(reinterpret_cast<const int64_t *>(v.GetData())[i]);
                                    else if (tid == LogicalTypeId::DOUBLE)
                                        k += std::to_string(reinterpret_cast<const double *>(v.GetData())[i]);
                                    else if (tid == LogicalTypeId::VARCHAR) {
                                        auto &s = reinterpret_cast<const string_t *>(v.GetData())[i];
                                        k.append(s.GetData(), s.GetSize());
                                    }
                                    k += '|';
                                }
                                auto it = ts.str_groups.find(k);
                                if (it == ts.str_groups.end()) {
                                    std::vector<Value> vals;
                                    vals.reserve(group_col_indices.size());
                                    for (idx_t gci : group_col_indices)
                                        vals.push_back(chunk.GetValue(gci, i));
                                    ts.str_keys_multi[k] = std::move(vals);
                                    it = ts.str_groups.emplace(std::move(k), std::vector<AggState>(num_aggs)).first;
                                }
                                states_ptr = &it->second;
                            } else if (is_int) {
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
            std::unordered_map<std::string, std::vector<Value>> str_keys_multi;

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
                        if (multi_group) {
                            auto it = ts.str_keys_multi.find(kv.first);
                            if (it != ts.str_keys_multi.end()) str_keys_multi[kv.first] = it->second;
                        } else {
                            str_keys[kv.first] = ts.str_keys[kv.first];
                        }
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
            if (multi_group) {
                for (auto &k : str_order) {
                    group_states[k] = std::move(str_groups[k]);
                    auto it = str_keys_multi.find(k);
                    if (it != str_keys_multi.end()) group_keys[k] = std::move(it->second);
                    group_order.push_back(k);
                }
            } else
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

            // Ultra-fast path: single VARCHAR group column + only COUNT(*) aggs.
            // Walk CSV buffer directly, no DataChunk materialization.
            bool ultrafast = false;
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                auto types_arr = children[0]->GetTypes();
                if (group_col < types_arr.size() &&
                    types_arr[group_col].id() == LogicalTypeId::VARCHAR) {
                    bool all_count_star = true;
                    for (auto &info : agg_infos) {
                        if (!(info.name == "COUNT" && info.is_count_star)) {
                            all_count_star = false;
                            break;
                        }
                    }
                    if (all_count_star && fs->GetReader()) {
                        ultrafast = true;
                        fs->Init();

                        struct CacheEntry {
                            std::string key;
                            int64_t count;
                        };
                        std::vector<CacheEntry> cache;
                        cache.reserve(256);

                        fs->GetReader()->ForEachVarcharCol(group_col, types_arr.size(),
                            [&](const char *d, size_t l) {
                                for (auto &e : cache) {
                                    if (e.key.size() == l && memcmp(e.key.data(), d, l) == 0) {
                                        e.count++;
                                        return;
                                    }
                                }
                                cache.push_back({std::string(d, l), 1});
                            });

                        // Move into standard map for result emit.
                        for (auto &e : cache) {
                            group_order.push_back(e.key);
                            group_keys[e.key] = {Value::VARCHAR(e.key)};
                            auto &st = group_states[e.key];
                            st.resize(num_aggs);
                            for (auto &s : st) s.count = e.count;
                        }
                    }
                }
            }
            if (!ultrafast) {
                // Column pruning: tell file scan or join to only populate needed columns.
                std::vector<bool> needed(children[0]->GetTypes().size(), false);
                needed[group_col] = true;
                for (auto &info : agg_infos) {
                    if (info.col_idx != INVALID_INDEX && info.col_idx < needed.size()) {
                        needed[info.col_idx] = true;
                    }
                }
                children[0]->SetNeededOutputs(needed);
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(children[0].get())) {
                fs->SetProjection(std::move(needed));
            }

            // Direct-pointer cache for VARCHAR group keys (low-cardinality optimization).
            // Reserve enough for typical cardinality — pointers remain stable.
            std::vector<std::string> str_cache_keys;
            std::vector<std::vector<AggState>> str_cache_states;
            str_cache_keys.reserve(256);
            str_cache_states.reserve(256);

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
                        const char *s_data = s.GetData();
                        uint32_t s_len = s.GetSize();
                        // Linear scan with direct pointer cache — no map lookup on hit.
                        states_ptr = nullptr;
                        for (idx_t oi = 0; oi < str_cache_keys.size(); oi++) {
                            const auto &ck = str_cache_keys[oi];
                            if (ck.size() == s_len && memcmp(ck.data(), s_data, s_len) == 0) {
                                states_ptr = &str_cache_states[oi];
                                break;
                            }
                        }
                        if (!states_ptr) {
                            // New key — add to cache and map.
                            std::string k(s_data, s_len);
                            str_cache_keys.push_back(k);
                            str_cache_states.emplace_back(num_aggs);
                            str_order.push_back(k);
                            str_keys[k] = chunk.GetValue(group_col, i);
                            states_ptr = &str_cache_states.back();
                        }
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

            // Move cache states into str_groups for result building.
            for (idx_t oi = 0; oi < str_cache_keys.size(); oi++) {
                str_groups[str_cache_keys[oi]] = std::move(str_cache_states[oi]);
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
            } // end !ultrafast
        } else if (is_simple_count_star) {
            // Ultra-fast: if child is a file scan, just count newlines.
            group_order.push_back("");
            group_states[""].resize(1);
            group_keys[""] = {};
            auto &state = group_states[""][0];

            auto *file_scan = dynamic_cast<PhysicalFileScan *>(children[0].get());
            auto *count_scan = dynamic_cast<PhysicalCountScan *>(children[0].get());
            auto *parquet_scan = dynamic_cast<PhysicalParquetScan *>(children[0].get());
            if (file_scan) {
                // Reuse the already-loaded reader — no second fread.
                auto *r = file_scan->GetReader();
                idx_t count = r->CountRows();
                state.count = static_cast<int64_t>(count);
                total_rows_processed = count;
            } else if (count_scan) {
                state.count = static_cast<int64_t>(count_scan->GetRowCount());
                total_rows_processed = count_scan->GetRowCount();
            } else if (parquet_scan) {
                // Parquet COUNT(*) — read row count from footer metadata without
                // decoding any column data.
                parquet_scan->Init();
                state.count = parquet_scan->GetReader()->NumRows();
                total_rows_processed = static_cast<idx_t>(state.count);
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

    // Projection pushdown: tell join which output columns are needed.
    void SetNeededOutputs(const std::vector<bool> &mask) override { needed_cols_ = mask; }

    void Init() override {
        for (auto &child : children) child->Init();
        built_ = false;
        emit_pos_ = 0;
        probe_done_ = false;
        probe_chunk_pos_ = 0;
        match_pos_ = 0;
        current_match_list_ = nullptr;
        current_probe_row_ = 0;
    }

    bool GetData(DataChunk &result) override {
        if (!built_) {
            BuildHashTable();
            built_ = true;
        }

        // CROSS join uses the legacy result_rows_ path.
        if (join_type_ == JoinType::CROSS) {
            if (emit_pos_ >= result_rows_.size()) return false;
            result.Initialize(GetTypes());
            idx_t count = 0;
            while (emit_pos_ < result_rows_.size() && count < VECTOR_SIZE) {
                auto &row = result_rows_[emit_pos_];
                for (idx_t col = 0; col < row.size(); col++) result.SetValue(col, count, row[col]);
                emit_pos_++;
                count++;
            }
            result.SetCardinality(count);
            return count > 0;
        }

        // Streaming probe: produce one chunk at a time.
        // Only initialize if chunk isn't already set up (reuse across calls).
        if (result.ColumnCount() != GetTypes().size()) {
            result.Initialize(GetTypes());
        } else {
            result.Reset();
        }
        idx_t out = 0;

        auto emit = [&](idx_t build_idx) {
            idx_t col = 0;
            auto set_if_needed = [&](const Value &v) {
                if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                    result.SetValue(col, out, v);
                }
                col++;
            };
            if (build_is_right_) {
                for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++) {
                    if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                        result.SetValue(col, out, probe_chunk_.GetValue(c, current_probe_row_));
                    }
                    col++;
                }
                for (auto &v : build_rows_[build_idx]) set_if_needed(v);
            } else {
                for (auto &v : build_rows_[build_idx]) set_if_needed(v);
                for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++) {
                    if (needed_cols_.empty() || (col < needed_cols_.size() && needed_cols_[col])) {
                        result.SetValue(col, out, probe_chunk_.GetValue(c, current_probe_row_));
                    }
                    col++;
                }
            }
            out++;
            build_matched_[build_idx] = true;
        };

        while (out < VECTOR_SIZE) {
            // Emit remaining matches from current probe row.
            if (current_match_list_) {
                while (out < VECTOR_SIZE && match_pos_ < current_match_list_->size()) {
                    emit((*current_match_list_)[match_pos_]);
                    match_pos_++;
                }
                if (match_pos_ >= current_match_list_->size()) {
                    current_match_list_ = nullptr;
                    probe_chunk_pos_++;
                    match_pos_ = 0;
                }
                if (out >= VECTOR_SIZE) break;
            }

            // Process next probe row in current chunk.
            if (probe_chunk_pos_ < probe_chunk_.size()) {
                current_probe_row_ = probe_chunk_pos_;
                auto &key_vec = probe_chunk_.GetVector(probe_join_col_);
                std::vector<idx_t> *match_list = nullptr;

                if (use_linear_cache_ && key_vec.GetType().id() == LogicalTypeId::VARCHAR) {
                    // Ultra-fast: direct bytes from string_t, linear memcmp.
                    auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[current_probe_row_];
                    const char *d = s.GetData();
                    uint32_t l = s.GetSize();
                    for (idx_t ki = 0; ki < build_key_cache_.size(); ki++) {
                        const auto &k = build_key_cache_[ki];
                        if (k.size() == l && memcmp(k.data(), d, l) == 0) {
                            match_list = build_match_cache_[ki];
                            break;
                        }
                    }
                } else {
                    std::string key;
                    if (key_vec.GetType().id() == LogicalTypeId::VARCHAR) {
                        auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[current_probe_row_];
                        key.assign(s.GetData(), s.GetSize());
                    } else {
                        key = probe_chunk_.GetValue(probe_join_col_, current_probe_row_).ToString();
                    }
                    auto it = hash_table_.find(key);
                    if (it != hash_table_.end()) match_list = &it->second;
                }

                if (match_list && !match_list->empty()) {
                    current_match_list_ = match_list;
                    match_pos_ = 0;
                } else {
                    // Outer join: emit probe row with NULLs for build side.
                    bool want = false;
                    if (!build_is_right_ && (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL)) want = true;
                    if (build_is_right_ && (join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL)) want = true;
                    if (want) {
                        idx_t col = 0;
                        if (build_is_right_) {
                            for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++)
                                result.SetValue(col++, out, probe_chunk_.GetValue(c, current_probe_row_));
                            for (idx_t c = 0; c < right_col_count_; c++) result.SetValue(col++, out, Value());
                        } else {
                            for (idx_t c = 0; c < left_col_count_; c++) result.SetValue(col++, out, Value());
                            for (idx_t c = 0; c < probe_chunk_.ColumnCount(); c++)
                                result.SetValue(col++, out, probe_chunk_.GetValue(c, current_probe_row_));
                        }
                        out++;
                    }
                    probe_chunk_pos_++;
                }
                continue;
            }

            // Current chunk exhausted — fetch next.
            if (probe_done_) break;
            probe_chunk_.Initialize(probe_child_->GetTypes());
            if (!probe_child_->GetData(probe_chunk_)) {
                probe_done_ = true;
                break;
            }
            probe_chunk_pos_ = 0;
        }

        // After probe is fully done, emit unmatched build rows for outer joins.
        if (probe_done_ && out < VECTOR_SIZE) {
            bool want = false;
            if (!build_is_right_ && (join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL)) want = true;
            if (build_is_right_ && (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL)) want = true;
            if (want) {
                while (out < VECTOR_SIZE && outer_emit_pos_ < build_rows_.size()) {
                    if (!build_matched_[outer_emit_pos_]) {
                        idx_t col = 0;
                        if (build_is_right_) {
                            for (idx_t c = 0; c < left_col_count_; c++) result.SetValue(col++, out, Value());
                            for (auto &v : build_rows_[outer_emit_pos_]) result.SetValue(col++, out, v);
                        } else {
                            for (auto &v : build_rows_[outer_emit_pos_]) result.SetValue(col++, out, v);
                            for (idx_t c = 0; c < right_col_count_; c++) result.SetValue(col++, out, Value());
                        }
                        out++;
                    }
                    outer_emit_pos_++;
                }
            }
        }

        result.SetCardinality(out);
        return out > 0;
    }

    // --- Fuse interface: used by PhysicalHashAggregate to bypass materialization. ---
    void FuseBuild() { if (!built_) { BuildHashTable(); built_ = true; } }
    bool FuseIsInner() const { return join_type_ == JoinType::INNER; }
    bool FuseBuildIsRight() const { return build_is_right_; }
    PhysicalOperator *FuseProbeChild() { return probe_child_; }
    const std::vector<std::vector<Value>> &FuseBuildRows() const { return build_rows_; }
    const std::unordered_map<std::string, std::vector<idx_t>> &FuseHashTable() const { return hash_table_; }
    bool FuseUseLinearCache() const { return use_linear_cache_; }
    const std::vector<std::string> &FuseBuildKeyCache() const { return build_key_cache_; }
    const std::vector<std::vector<idx_t>*> &FuseBuildMatchCache() const { return build_match_cache_; }
    idx_t FuseProbeJoinCol() const { return probe_join_col_; }
    idx_t FuseLeftColCount() const { return left_col_count_; }

private:
    void BuildHashTable() {
        DataChunk chunk;

        // CROSS JOIN: materialize both sides, emit all pairs into result_rows_ (legacy path).
        if (join_type_ == JoinType::CROSS) {
            std::vector<std::vector<Value>> l_rows, r_rows;
            while (true) {
                chunk.Initialize(children[0]->GetTypes());
                if (!children[0]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        row.push_back(chunk.GetValue(c, i));
                    l_rows.push_back(std::move(row));
                }
            }
            while (true) {
                chunk.Initialize(children[1]->GetTypes());
                if (!children[1]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                        row.push_back(chunk.GetValue(c, i));
                    r_rows.push_back(std::move(row));
                }
            }
            for (auto &lr : l_rows) {
                for (auto &rr : r_rows) {
                    std::vector<Value> combined;
                    for (auto &v : lr) combined.push_back(v);
                    for (auto &v : rr) combined.push_back(v);
                    result_rows_.push_back(std::move(combined));
                }
            }
            return;
        }

        // Try to materialize LEFT side with a size guard.
        // If LEFT exceeds threshold, bail and use RIGHT as build side instead.
        static constexpr idx_t SMALL_SIDE_THRESHOLD = 100000;
        std::vector<std::vector<Value>> left_rows;
        bool left_too_big = false;
        while (!left_too_big) {
            chunk.Initialize(children[0]->GetTypes());
            if (!children[0]->GetData(chunk)) break;
            for (idx_t i = 0; i < chunk.size(); i++) {
                std::vector<Value> row;
                for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                    row.push_back(chunk.GetValue(col, i));
                left_rows.push_back(std::move(row));
                if (left_rows.size() > SMALL_SIDE_THRESHOLD) {
                    left_too_big = true;
                    break;
                }
            }
        }

        // If LEFT too big, switch: materialize RIGHT as build side instead.
        bool build_is_right = false;
        std::vector<std::vector<Value>> build_rows;
        if (left_too_big) {
            children[0]->Init(); // reset left scan
            build_is_right = true;
            while (true) {
                chunk.Initialize(children[1]->GetTypes());
                if (!children[1]->GetData(chunk)) break;
                for (idx_t i = 0; i < chunk.size(); i++) {
                    std::vector<Value> row;
                    for (idx_t col = 0; col < chunk.ColumnCount(); col++)
                        row.push_back(chunk.GetValue(col, i));
                    build_rows.push_back(std::move(row));
                }
            }
            left_rows.clear();
        } else {
            build_rows = std::move(left_rows);
        }

        // Extract join column indices from condition.
        idx_t left_join_col = 0, right_join_col = 0;
        if (condition_ && condition_->GetExpressionType() == BoundExpressionType::COMPARISON) {
            auto &cmp = static_cast<BoundComparison &>(*condition_);
            if (cmp.left->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                left_join_col = static_cast<BoundColumnRef &>(*cmp.left).column_index;
            }
            if (cmp.right->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                idx_t combined = static_cast<BoundColumnRef &>(*cmp.right).column_index;
                if (combined >= left_col_count_) {
                    right_join_col = combined - left_col_count_;
                } else {
                    right_join_col = combined;
                }
            }
        }

        // Store state for streaming probe.
        build_rows_ = std::move(build_rows);
        build_is_right_ = build_is_right;
        probe_join_col_ = build_is_right ? left_join_col : right_join_col;
        probe_child_ = build_is_right ? children[0].get() : children[1].get();
        build_matched_.assign(build_rows_.size(), false);
        hash_table_.clear();
        build_key_cache_.clear();
        build_match_cache_.clear();
        idx_t build_join_col = build_is_right ? right_join_col : left_join_col;
        for (idx_t i = 0; i < build_rows_.size(); i++) {
            auto key = build_rows_[i][build_join_col].ToString();
            hash_table_[key].push_back(i);
        }
        // Build a linear cache for fast string-probe lookups — faster than hash map
        // for small build sides (like dimension tables with <256 unique keys).
        use_linear_cache_ = build_rows_.size() <= 256;
        if (use_linear_cache_) {
            for (auto &kv : hash_table_) {
                build_key_cache_.push_back(kv.first);
                build_match_cache_.push_back(&kv.second);
            }
        }

        // Push projection down to probe child: only the join column + columns
        // that appear in the output and are needed by the caller.
        if (!needed_cols_.empty()) {
            idx_t probe_col_count = probe_child_->GetTypes().size();
            std::vector<bool> probe_needed(probe_col_count, false);
            probe_needed[probe_join_col_] = true;
            // Map each output column → probe column if applicable.
            for (idx_t out_col = 0; out_col < needed_cols_.size() && needed_cols_[out_col]; out_col++) {}
            for (idx_t out_col = 0; out_col < needed_cols_.size(); out_col++) {
                if (!needed_cols_[out_col]) continue;
                idx_t probe_col = INVALID_INDEX;
                if (build_is_right_) {
                    // output = [probe_cols..., build_cols...]
                    if (out_col < probe_col_count) probe_col = out_col;
                } else {
                    // output = [build_cols..., probe_cols...]
                    if (out_col >= left_col_count_) probe_col = out_col - left_col_count_;
                }
                if (probe_col != INVALID_INDEX && probe_col < probe_col_count) {
                    probe_needed[probe_col] = true;
                }
            }
            probe_child_->SetNeededOutputs(probe_needed);
            // If probe child is a file scan, also set CSV projection.
            if (auto *fs = dynamic_cast<PhysicalFileScan *>(probe_child_)) {
                fs->SetProjection(probe_needed);
            }
        }
    }

private:
    // Member state for streaming probe.
    bool build_is_right_ = false;
    std::vector<std::vector<Value>> build_rows_;
    std::unordered_map<std::string, std::vector<idx_t>> hash_table_;
    // Linear cache for low-cardinality probes — avoids map/string hash overhead.
    bool use_linear_cache_ = false;
    std::vector<std::string> build_key_cache_;
    std::vector<std::vector<idx_t>*> build_match_cache_;
    std::vector<bool> build_matched_;
    idx_t probe_join_col_ = 0;
    PhysicalOperator *probe_child_ = nullptr;
    DataChunk probe_chunk_;
    idx_t probe_chunk_pos_ = 0;
    idx_t current_probe_row_ = 0;
    const std::vector<idx_t> *current_match_list_ = nullptr;
    idx_t match_pos_ = 0;
    bool probe_done_ = false;
    idx_t outer_emit_pos_ = 0;

    JoinType join_type_;
    BoundExprPtr condition_;
    idx_t left_col_count_;
    idx_t right_col_count_;
    std::vector<std::vector<Value>> result_rows_; // only for CROSS
    bool built_ = false;
    idx_t emit_pos_ = 0;
    std::vector<bool> needed_cols_;
};

static PhysicalHashJoin *AsHashJoin(PhysicalOperator *op) {
    return dynamic_cast<PhysicalHashJoin *>(op);
}

// Fused JOIN+aggregate hot path — streams probe chunks, looks up build side,
// updates aggregate state directly. Cuts 2-4x off large-fact × small-dim patterns
// (e.g. `sales JOIN regions GROUP BY manager`).
bool PhysicalHashAggregate::TryComputeFusedJoinAggregate(
    PhysicalHashJoin *hj,
    const std::vector<AggInfo> &agg_infos,
    const std::vector<idx_t> &group_col_indices) {
    if (!hj->FuseIsInner() || group_col_indices.empty()) return false;
    idx_t num_aggs = agg_infos.size();
    for (auto &info : agg_infos) {
        if (info.name == "COUNT" && info.is_count_star) continue;
        if ((info.name == "COUNT" || info.name == "SUM" || info.name == "AVG" ||
             info.name == "MIN" || info.name == "MAX") &&
            info.col_idx != INVALID_INDEX && !info.is_distinct) continue;
        return false;
    }

    hj->FuseBuild();
    auto *probe = hj->FuseProbeChild();
    const auto &build_rows = hj->FuseBuildRows();
    const auto &hash_tab = hj->FuseHashTable();
    bool build_is_right = hj->FuseBuildIsRight();
    idx_t probe_join_col = hj->FuseProbeJoinCol();
    idx_t left_cols = hj->FuseLeftColCount();
    idx_t probe_cols = probe->GetTypes().size();
    bool linear = hj->FuseUseLinearCache();
    const auto &key_cache = hj->FuseBuildKeyCache();
    const auto &match_cache = hj->FuseBuildMatchCache();

    auto map_col = [&](idx_t combined) -> std::pair<bool, idx_t> {
        if (build_is_right) {
            if (combined < probe_cols) return {false, combined};
            return {true, combined - probe_cols};
        } else {
            if (combined < left_cols) return {true, combined};
            return {false, combined - left_cols};
        }
    };

    // Push projection to the probe scan — only parse cols we actually read.
    {
        std::vector<bool> probe_needed(probe_cols, false);
        probe_needed[probe_join_col] = true;
        for (idx_t gc : group_col_indices) {
            auto pr = map_col(gc);
            if (!pr.first && pr.second < probe_cols) probe_needed[pr.second] = true;
        }
        for (auto &info : agg_infos) {
            if (info.col_idx == INVALID_INDEX) continue;
            auto pr = map_col(info.col_idx);
            if (!pr.first && pr.second < probe_cols) probe_needed[pr.second] = true;
        }
        probe->SetNeededOutputs(probe_needed);
        if (auto *fs = dynamic_cast<PhysicalFileScan *>(probe)) fs->SetProjection(probe_needed);
    }

    struct GKey { bool is_build; idx_t local; LogicalTypeId tid; };
    std::vector<GKey> gkeys;
    gkeys.reserve(group_col_indices.size());
    for (idx_t gc : group_col_indices) {
        auto pr = map_col(gc);
        LogicalTypeId tid = LogicalTypeId::SQLNULL;
        if (pr.first) {
            if (!build_rows.empty() && pr.second < build_rows[0].size())
                tid = build_rows[0][pr.second].type().id();
        } else {
            if (pr.second < probe->GetTypes().size())
                tid = probe->GetTypes()[pr.second].id();
        }
        gkeys.push_back({pr.first, pr.second, tid});
    }

    struct ACol { bool is_build; idx_t local; };
    std::vector<ACol> acols(num_aggs);
    for (idx_t a = 0; a < num_aggs; a++) {
        if (agg_infos[a].col_idx == INVALID_INDEX) { acols[a] = {false, INVALID_INDEX}; continue; }
        auto pr = map_col(agg_infos[a].col_idx);
        acols[a] = {pr.first, pr.second};
    }

    // Precompute per-build-row group key if all group cols are build-side.
    bool all_build_gkeys = true;
    for (auto &g : gkeys) if (!g.is_build) { all_build_gkeys = false; break; }

    // Fast path: when GROUP BY is all build-side, map each build_idx → group_idx
    // directly (array index instead of hashing a string per probe row).
    std::vector<idx_t> build_to_gidx;
    std::vector<std::vector<Value>> group_vals_by_gidx;
    std::vector<std::vector<AggState>> states_by_gidx;
    if (all_build_gkeys) {
        std::unordered_map<std::string, idx_t> gkey_to_gidx;
        build_to_gidx.assign(build_rows.size(), INVALID_INDEX);
        for (idx_t i = 0; i < build_rows.size(); i++) {
            std::string gkey;
            std::vector<Value> gvals(gkeys.size());
            for (idx_t j = 0; j < gkeys.size(); j++) {
                auto &v = build_rows[i][gkeys[j].local];
                gvals[j] = v;
                if (v.IsNull()) gkey += "\x01N"; else gkey += v.ToString();
                gkey += '|';
            }
            auto it = gkey_to_gidx.find(gkey);
            idx_t gidx;
            if (it == gkey_to_gidx.end()) {
                gidx = group_vals_by_gidx.size();
                group_vals_by_gidx.push_back(std::move(gvals));
                states_by_gidx.emplace_back(num_aggs);
                gkey_to_gidx.emplace(std::move(gkey), gidx);
            } else {
                gidx = it->second;
            }
            build_to_gidx[i] = gidx;
        }
    }

    std::unordered_map<std::string, std::vector<AggState>> group_states;
    std::unordered_map<std::string, std::vector<Value>> group_keys_map;
    std::vector<std::string> group_order;

    // PARALLEL PATH: large file + build-side GROUP BY → split into N thread slices.
    // Each thread does full parse + probe + local aggregate into its own state array,
    // then merge. Eliminates the single-threaded CSV parse bottleneck.
    bool parallel_done = false;
    if (all_build_gkeys) {
        auto *fs = dynamic_cast<PhysicalFileScan *>(probe);
        FastCSVReader *reader = fs ? fs->GetReader() : nullptr;
        if (reader && reader->GetBuffer() && reader->GetSize() > 16 * 1024 * 1024) {
            const char *buffer = reader->GetBuffer();
            size_t total_size = reader->GetSize();
            size_t data_start = reader->GetPos();
            size_t data_size = total_size - data_start;

            unsigned int nt = std::thread::hardware_concurrency();
            if (nt == 0) nt = 4;
            if (nt > 8) nt = 8;
            std::vector<size_t> ranges(nt + 1);
            ranges[0] = data_start;
            ranges[nt] = total_size;
            for (unsigned int t = 1; t < nt; t++) {
                size_t target = data_start + (data_size * t) / nt;
                ranges[t] = FastCSVReader::FindLineStart(buffer, total_size, target);
            }

            idx_t num_groups = states_by_gidx.size();
            std::vector<std::vector<std::vector<AggState>>> thread_states(nt);
            for (auto &ts : thread_states) {
                ts.resize(num_groups);
                for (auto &g : ts) g.resize(num_aggs);
            }

            char delim = fs->GetDelimiter();
            auto types = probe->GetTypes();
            auto projection = fs->GetProjection(); // copy once

            std::vector<std::thread> threads;
            for (unsigned int t = 0; t < nt; t++) {
                threads.emplace_back([&, t]() {
                    FastCSVReader tr(buffer, ranges[t], ranges[t + 1], delim);
                    DataChunk chunk;
                    chunk.Initialize(types);
                    auto &ts = thread_states[t];
                    while (true) {
                        chunk.Reset();
                        idx_t cnt = projection.empty()
                            ? tr.ReadChunk(chunk, types)
                            : tr.ReadChunkProjected(chunk, types, projection);
                        if (cnt == 0) break;
                        auto &kvec = chunk.GetVector(probe_join_col);
                        auto ktid = kvec.GetType().id();
                        for (idx_t i = 0; i < cnt; i++) {
                            const std::vector<idx_t> *ml = nullptr;
                            if (linear && ktid == LogicalTypeId::VARCHAR) {
                                auto &s = reinterpret_cast<const string_t *>(kvec.GetData())[i];
                                const char *d = s.GetData(); uint32_t l = s.GetSize();
                                for (idx_t ki = 0; ki < key_cache.size(); ki++) {
                                    const auto &k = key_cache[ki];
                                    if (k.size() == l && memcmp(k.data(), d, l) == 0) { ml = match_cache[ki]; break; }
                                }
                            } else {
                                std::string ks;
                                if (ktid == LogicalTypeId::VARCHAR) {
                                    auto &s = reinterpret_cast<const string_t *>(kvec.GetData())[i];
                                    ks.assign(s.GetData(), s.GetSize());
                                } else {
                                    ks = chunk.GetValue(probe_join_col, i).ToString();
                                }
                                auto it = hash_tab.find(ks);
                                if (it != hash_tab.end()) ml = &it->second;
                            }
                            if (!ml || ml->empty()) continue;
                            for (idx_t build_idx : *ml) {
                                idx_t gidx = build_to_gidx[build_idx];
                                auto &states = ts[gidx];
                                auto &br = build_rows[build_idx];
                                for (idx_t a = 0; a < num_aggs; a++) {
                                    auto &state = states[a];
                                    auto &info = agg_infos[a];
                                    if (info.name == "COUNT" && info.is_count_star) { state.count++; continue; }
                                    auto &ac = acols[a];
                                    if (ac.local == INVALID_INDEX) continue;
                                    if (ac.is_build) {
                                        auto &v = br[ac.local];
                                        if (v.IsNull()) continue;
                                        if (info.name == "COUNT") { state.count++; continue; }
                                        double d = 0;
                                        auto tid = v.type().id();
                                        if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                                        else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                                        else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                                        else if (info.name == "MIN") { if (!state.has_min || d < state.sum_min) { state.sum_min = d; state.has_min = true; } }
                                        else if (info.name == "MAX") { if (!state.has_max || d > state.sum_max) { state.sum_max = d; state.has_max = true; } }
                                    } else {
                                        auto &vec = chunk.GetVector(ac.local);
                                        if (!vec.GetValidity().RowIsValid(i)) continue;
                                        auto tid = vec.GetType().id();
                                        if (info.name == "COUNT") { state.count++; continue; }
                                        double d = 0;
                                        if (tid == LogicalTypeId::INTEGER)
                                            d = reinterpret_cast<const int32_t *>(vec.GetData())[i];
                                        else if (tid == LogicalTypeId::BIGINT)
                                            d = static_cast<double>(reinterpret_cast<const int64_t *>(vec.GetData())[i]);
                                        else if (tid == LogicalTypeId::DOUBLE)
                                            d = reinterpret_cast<const double *>(vec.GetData())[i];
                                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                                        else if (info.name == "MIN") { if (!state.has_min || d < state.sum_min) { state.sum_min = d; state.has_min = true; } }
                                        else if (info.name == "MAX") { if (!state.has_max || d > state.sum_max) { state.sum_max = d; state.has_max = true; } }
                                    }
                                }
                            }
                        }
                    }
                });
            }
            for (auto &th : threads) th.join();

            // Merge thread-local into final states_by_gidx.
            for (unsigned int t = 0; t < nt; t++) {
                for (idx_t g = 0; g < num_groups; g++) {
                    for (idx_t a = 0; a < num_aggs; a++) {
                        auto &F = states_by_gidx[g][a];
                        auto &P = thread_states[t][g][a];
                        F.count += P.count;
                        F.sum += P.sum;
                        if (P.has_min && (!F.has_min || P.sum_min < F.sum_min)) { F.sum_min = P.sum_min; F.has_min = true; }
                        if (P.has_max && (!F.has_max || P.sum_max > F.sum_max)) { F.sum_max = P.sum_max; F.has_max = true; }
                    }
                }
            }
            parallel_done = true;
        }
    }

    DataChunk pchunk;
    while (!parallel_done && probe->GetData(pchunk)) {
        idx_t cnt = pchunk.size();
        auto &key_vec = pchunk.GetVector(probe_join_col);
        auto key_tid = key_vec.GetType().id();
        for (idx_t i = 0; i < cnt; i++) {
            const std::vector<idx_t> *match_list = nullptr;
            if (linear && key_tid == LogicalTypeId::VARCHAR) {
                auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[i];
                const char *d = s.GetData();
                uint32_t l = s.GetSize();
                for (idx_t ki = 0; ki < key_cache.size(); ki++) {
                    const auto &k = key_cache[ki];
                    if (k.size() == l && memcmp(k.data(), d, l) == 0) {
                        match_list = match_cache[ki];
                        break;
                    }
                }
            } else {
                std::string key_s;
                if (key_tid == LogicalTypeId::VARCHAR) {
                    auto &s = reinterpret_cast<const string_t *>(key_vec.GetData())[i];
                    key_s.assign(s.GetData(), s.GetSize());
                } else {
                    key_s = pchunk.GetValue(probe_join_col, i).ToString();
                }
                auto it = hash_tab.find(key_s);
                if (it != hash_tab.end()) match_list = &it->second;
            }
            if (!match_list || match_list->empty()) continue;

            for (idx_t build_idx : *match_list) {
                auto &br = build_rows[build_idx];
                std::vector<AggState> *states_ptr;
                if (all_build_gkeys) {
                    // Direct array indexing — no string hashing in the hot loop.
                    idx_t gidx = build_to_gidx[build_idx];
                    states_ptr = &states_by_gidx[gidx];
                } else {
                    std::string gkey;
                    for (auto &g : gkeys) {
                        if (g.is_build) {
                            auto &v = br[g.local];
                            if (v.IsNull()) gkey += "\x01N"; else gkey += v.ToString();
                        } else {
                            auto v = pchunk.GetValue(g.local, i);
                            if (v.IsNull()) gkey += "\x01N"; else gkey += v.ToString();
                        }
                        gkey += '|';
                    }
                    auto git = group_states.find(gkey);
                    if (git == group_states.end()) {
                        std::vector<Value> gvals(gkeys.size());
                        for (idx_t j = 0; j < gkeys.size(); j++) {
                            if (gkeys[j].is_build) gvals[j] = br[gkeys[j].local];
                            else gvals[j] = pchunk.GetValue(gkeys[j].local, i);
                        }
                        group_keys_map[gkey] = std::move(gvals);
                        group_order.push_back(gkey);
                        git = group_states.emplace(gkey, std::vector<AggState>(num_aggs)).first;
                    }
                    states_ptr = &git->second;
                }
                auto &states = *states_ptr;

                for (idx_t a = 0; a < num_aggs; a++) {
                    auto &state = states[a];
                    auto &info = agg_infos[a];
                    if (info.name == "COUNT" && info.is_count_star) {
                        state.count++;
                        continue;
                    }
                    auto &ac = acols[a];
                    if (ac.local == INVALID_INDEX) continue;
                    if (ac.is_build) {
                        auto &v = br[ac.local];
                        if (v.IsNull()) continue;
                        if (info.name == "COUNT") { state.count++; continue; }
                        double d = 0;
                        auto tid = v.type().id();
                        if (tid == LogicalTypeId::INTEGER) d = v.GetValue<int32_t>();
                        else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(v.GetValue<int64_t>());
                        else if (tid == LogicalTypeId::DOUBLE) d = v.GetValue<double>();
                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                        else if (info.name == "MIN") { if (!state.has_min || v < state.min_val) { state.min_val = v; state.has_min = true; } }
                        else if (info.name == "MAX") { if (!state.has_max || v > state.max_val) { state.max_val = v; state.has_max = true; } }
                    } else {
                        auto &vec = pchunk.GetVector(ac.local);
                        if (!vec.GetValidity().RowIsValid(i)) continue;
                        auto tid = vec.GetType().id();
                        if (info.name == "COUNT") { state.count++; continue; }
                        double d = 0;
                        if (tid == LogicalTypeId::INTEGER)
                            d = reinterpret_cast<const int32_t *>(vec.GetData())[i];
                        else if (tid == LogicalTypeId::BIGINT)
                            d = static_cast<double>(reinterpret_cast<const int64_t *>(vec.GetData())[i]);
                        else if (tid == LogicalTypeId::DOUBLE)
                            d = reinterpret_cast<const double *>(vec.GetData())[i];
                        if (info.name == "SUM" || info.name == "AVG") { state.count++; state.sum += d; }
                        else if (info.name == "MIN") {
                            if (!state.has_min || d < state.sum_min) { state.sum_min = d; state.has_min = true; }
                        }
                        else if (info.name == "MAX") {
                            if (!state.has_max || d > state.sum_max) { state.sum_max = d; state.has_max = true; }
                        }
                    }
                }
            }
        }
    }

    // Normalize emit source: for the build-indexed fast path, feed
    // group_vals_by_gidx + states_by_gidx into the same emit loop.
    std::vector<std::vector<Value>> *emit_group_vals;
    std::vector<std::vector<AggState>> *emit_states;
    std::vector<std::vector<Value>> fallback_vals;
    std::vector<std::vector<AggState>> fallback_states;
    if (all_build_gkeys) {
        emit_group_vals = &group_vals_by_gidx;
        emit_states = &states_by_gidx;
    } else {
        fallback_vals.reserve(group_order.size());
        fallback_states.reserve(group_order.size());
        for (auto &gk : group_order) {
            fallback_vals.push_back(std::move(group_keys_map[gk]));
            fallback_states.push_back(std::move(group_states[gk]));
        }
        emit_group_vals = &fallback_vals;
        emit_states = &fallback_states;
    }
    for (idx_t gi = 0; gi < emit_group_vals->size(); gi++) {
        auto &states = (*emit_states)[gi];
        auto &gvals = (*emit_group_vals)[gi];
        std::vector<Value> row;
        row.reserve(gkeys.size() + num_aggs);
        for (auto &v : gvals) row.push_back(v);
        for (idx_t a = 0; a < num_aggs; a++) {
            auto &state = states[a];
            auto &info = agg_infos[a];
            Value r;
            if (info.name == "COUNT") r = Value::BIGINT(state.count);
            else if (info.name == "SUM") {
                auto &ac = acols[a];
                bool is_double = false;
                if (ac.local != INVALID_INDEX) {
                    if (ac.is_build) {
                        if (!build_rows.empty() && ac.local < build_rows[0].size())
                            is_double = build_rows[0][ac.local].type().id() == LogicalTypeId::DOUBLE;
                    } else {
                        is_double = probe->GetTypes()[ac.local].id() == LogicalTypeId::DOUBLE;
                    }
                }
                r = is_double ? Value::DOUBLE(state.sum) : Value::BIGINT(static_cast<int64_t>(state.sum));
            }
            else if (info.name == "AVG") r = state.count > 0 ? Value::DOUBLE(state.sum / state.count) : Value();
            else if (info.name == "MIN") {
                auto &ac = acols[a];
                if (!state.has_min) r = Value();
                else if (ac.is_build) r = state.min_val;
                else {
                    auto tid = probe->GetTypes()[ac.local].id();
                    if (tid == LogicalTypeId::INTEGER) r = Value::INTEGER(static_cast<int32_t>(state.sum_min));
                    else if (tid == LogicalTypeId::BIGINT) r = Value::BIGINT(static_cast<int64_t>(state.sum_min));
                    else r = Value::DOUBLE(state.sum_min);
                }
            }
            else if (info.name == "MAX") {
                auto &ac = acols[a];
                if (!state.has_max) r = Value();
                else if (ac.is_build) r = state.max_val;
                else {
                    auto tid = probe->GetTypes()[ac.local].id();
                    if (tid == LogicalTypeId::INTEGER) r = Value::INTEGER(static_cast<int32_t>(state.sum_max));
                    else if (tid == LogicalTypeId::BIGINT) r = Value::BIGINT(static_cast<int64_t>(state.sum_max));
                    else r = Value::DOUBLE(state.sum_max);
                }
            }
            row.push_back(r);
        }
        result_rows_.push_back(std::move(row));
    }
    return true;
}

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
    if (op.table && op.table->IsFileScan()) {
        if (op.table->GetFileFormat() == "parquet") {
            return std::make_unique<PhysicalParquetScan>(
                op.table->GetFilePath(), op.table->GetTypes());
        }
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

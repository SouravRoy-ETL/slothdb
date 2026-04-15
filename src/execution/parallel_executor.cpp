#include "slothdb/execution/parallel_executor.hpp"
#include "slothdb/common/exception.hpp"

namespace slothdb {

void ParallelResultCollector::AddChunk(const DataChunk &chunk) {
    std::lock_guard<std::mutex> guard(lock_);
    for (idx_t i = 0; i < chunk.size(); i++) {
        std::vector<Value> row;
        for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
            row.push_back(chunk.GetValue(col, i));
        }
        rows_.push_back(std::move(row));
    }
}

std::vector<std::vector<Value>> ParallelResultCollector::GetRows() const {
    return rows_;
}

idx_t ParallelResultCollector::RowCount() const {
    return static_cast<idx_t>(rows_.size());
}

std::vector<std::vector<Value>> ParallelExecutor::ParallelScanFilter(
    DataTable &table,
    const BoundExpression *filter,
    const std::vector<BoundExprPtr> *projections,
    const std::vector<LogicalType> &result_types,
    unsigned int num_threads) {

    auto &pool = ThreadPool::GetGlobal();
    if (num_threads == 0) num_threads = pool.NumThreads();

    // If table is small (< 2 row groups), just do single-threaded.
    if (table.RowGroupCount() <= 1) {
        num_threads = 1;
    }

    ParallelResultCollector collector;
    auto scan_state = table.InitParallelScan();
    auto &table_types = table.GetTypes();
    auto &scan_ref = *scan_state;

    // Launch worker tasks.
    std::vector<std::future<void>> futures;
    for (unsigned int t = 0; t < num_threads; t++) {
        futures.push_back(pool.Submit([&]() {
            DataChunk input;
            while (table.ParallelScan(scan_ref, input)) {
                if (input.size() == 0) continue;

                // Apply filter if present.
                DataChunk filtered;
                if (filter) {
                    Vector filter_result(LogicalType::BOOLEAN(), input.size());
                    ExpressionExecutor::Execute(*filter, input, filter_result, input.size());
                    auto *filter_data = filter_result.GetData<bool>();

                    filtered.Initialize(table_types);
                    idx_t result_count = 0;
                    for (idx_t i = 0; i < input.size(); i++) {
                        if (filter_result.GetValidity().RowIsValid(i) && filter_data[i]) {
                            for (idx_t col = 0; col < input.ColumnCount(); col++) {
                                filtered.SetValue(col, result_count, input.GetValue(col, i));
                            }
                            result_count++;
                        }
                    }
                    filtered.SetCardinality(result_count);
                    if (result_count == 0) continue;
                } else {
                    filtered = std::move(input);
                }

                // Apply projection if present.
                if (projections && !projections->empty()) {
                    DataChunk projected;
                    projected.Initialize(result_types);
                    for (idx_t col = 0; col < projections->size(); col++) {
                        ExpressionExecutor::Execute(*(*projections)[col], filtered,
                                                    projected.GetVector(col), filtered.size());
                    }
                    projected.SetCardinality(filtered.size());
                    collector.AddChunk(projected);
                } else {
                    collector.AddChunk(filtered);
                }
            }
        }));
    }

    // Wait for all tasks to complete.
    for (auto &f : futures) {
        f.get();
    }

    return collector.GetRows();
}

} // namespace slothdb

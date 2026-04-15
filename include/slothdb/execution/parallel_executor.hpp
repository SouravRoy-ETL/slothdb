#pragma once

#include "slothdb/common/thread_pool.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/storage/data_table.hpp"
#include "slothdb/execution/expression_executor.hpp"
#include "slothdb/binder/bound_expression.hpp"
#include <functional>
#include <mutex>
#include <vector>

namespace slothdb {

// Result collector: thread-safe accumulation of result rows.
class ParallelResultCollector {
public:
    void AddChunk(const DataChunk &chunk);
    std::vector<std::vector<Value>> GetRows() const;
    idx_t RowCount() const;

private:
    mutable std::mutex lock_;
    std::vector<std::vector<Value>> rows_;
};

// Parallel executor for scan + filter + projection pipelines.
// Distributes row groups across threads as morsels.
class ParallelExecutor {
public:
    // Execute a parallel scan with optional filter and projection.
    // Returns collected results.
    static std::vector<std::vector<Value>> ParallelScanFilter(
        DataTable &table,
        const BoundExpression *filter,  // nullptr = no filter
        const std::vector<BoundExprPtr> *projections,  // nullptr = pass-through
        const std::vector<LogicalType> &result_types,
        unsigned int num_threads = 0);
};

} // namespace slothdb

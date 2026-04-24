#pragma once

#include "slothdb/storage/row_group.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <memory>

namespace slothdb {

// Scan state for iterating over a DataTable.
struct TableScanState {
    idx_t current_row_group = 0;
    idx_t current_row_in_group = 0;
};

// The physical storage for a table. Manages a collection of RowGroups.
class DataTable {
public:
    explicit DataTable(const std::vector<LogicalType> &types);

    // Append a DataChunk of data. May span multiple row groups.
    void Append(DataChunk &chunk);

    // Splice pre-built row groups into the table in bulk - used by bulk
    // loaders that assemble row groups in worker threads to avoid the
    // DataTable mutex on the hot path. Each RG's start_row_ is rewritten
    // to reflect its position in the table.
    void AppendRowGroups(std::vector<std::unique_ptr<RowGroup>> groups);

    // Initialize a scan.
    TableScanState InitScan() const;

    // Get the next chunk of data. Returns false when there's no more data.
    bool Scan(TableScanState &state, DataChunk &result) const;

    // Total number of rows in the table.
    idx_t Count() const { return total_rows_; }

    // Column types.
    const std::vector<LogicalType> &GetTypes() const { return types_; }
    idx_t ColumnCount() const { return static_cast<idx_t>(types_.size()); }

    // Parallel scan: distribute row groups as morsels across threads.
    // Each call atomically grabs the next morsel. Returns false when done.
    struct ParallelScanState {
        std::atomic<idx_t> next_morsel{0};
        idx_t total_morsels = 0;  // Pre-computed.
        // Non-copyable due to atomic.
        ParallelScanState() = default;
        ParallelScanState(const ParallelScanState &) = delete;
        ParallelScanState &operator=(const ParallelScanState &) = delete;
    };

    std::unique_ptr<ParallelScanState> InitParallelScan() const;
    // Each morsel = one VECTOR_SIZE chunk within a row group.
    bool ParallelScan(ParallelScanState &state, DataChunk &result) const;

    // Number of row groups (for parallel scheduling).
    idx_t RowGroupCount() const { return static_cast<idx_t>(row_groups_.size()); }

private:
    std::vector<LogicalType> types_;
    std::vector<std::unique_ptr<RowGroup>> row_groups_;
    idx_t total_rows_;
    mutable std::mutex lock_;
};

} // namespace slothdb

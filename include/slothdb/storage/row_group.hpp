#pragma once

#include "slothdb/storage/column_data.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include <vector>
#include <memory>

namespace slothdb {

// A RowGroup holds up to ROW_GROUP_SIZE rows of columnar data.
class RowGroup {
public:
    RowGroup(const std::vector<LogicalType> &types, idx_t start_row);

    idx_t Count() const { return count_; }
    idx_t StartRow() const { return start_row_; }
    bool IsFull() const { return count_ >= ROW_GROUP_SIZE; }
    idx_t Remaining() const { return ROW_GROUP_SIZE - count_; }

    // Append a DataChunk into this row group. Returns number of rows actually appended.
    idx_t Append(DataChunk &chunk, idx_t offset);

    // Scan rows [start, start + count) into a DataChunk.
    void Scan(DataChunk &result, idx_t start, idx_t scan_count) const;

    // Number of columns.
    idx_t ColumnCount() const { return static_cast<idx_t>(columns_.size()); }

private:
    idx_t start_row_;     // Global row offset of this row group.
    idx_t count_;         // Current number of rows.
    std::vector<std::unique_ptr<ColumnData>> columns_;
};

} // namespace slothdb

#include "slothdb/storage/row_group.hpp"
#include "slothdb/common/exception.hpp"
#include <algorithm>

namespace slothdb {

RowGroup::RowGroup(const std::vector<LogicalType> &types, idx_t start_row)
    : start_row_(start_row), count_(0) {
    columns_.reserve(types.size());
    for (auto &type : types) {
        columns_.push_back(std::make_unique<ColumnData>(type, ROW_GROUP_SIZE));
    }
}

idx_t RowGroup::Append(DataChunk &chunk, idx_t offset) {
    idx_t rows_to_append = std::min(chunk.size() - offset, Remaining());
    if (rows_to_append == 0) return 0;

    // Zero-copy path — the new AppendSlice takes the source vector+offset
    // directly. For VARCHAR it memcpys the 16-byte string_t entries and
    // shares the source's string buffer via shared_ptr, avoiding the two
    // std::string allocations per cell that the old per-value path did.
    for (idx_t col = 0; col < ColumnCount(); col++) {
        columns_[col]->AppendSlice(chunk.GetVector(col), offset, rows_to_append);
    }

    count_ += rows_to_append;
    return rows_to_append;
}

void RowGroup::Scan(DataChunk &result, idx_t start, idx_t scan_count) const {
    for (idx_t col = 0; col < ColumnCount(); col++) {
        columns_[col]->Scan(result.GetVector(col), start, scan_count);
    }
    result.SetCardinality(scan_count);
}

} // namespace slothdb

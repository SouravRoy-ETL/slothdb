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

    for (idx_t col = 0; col < ColumnCount(); col++) {
        // Create a temporary vector with the slice we want to append.
        auto &src_vec = chunk.GetVector(col);
        auto physical = src_vec.GetType().GetInternalType();
        idx_t type_size = GetTypeIdSize(physical);

        // Build a temporary vector pointing to the offset data.
        Vector temp(src_vec.GetType(), VECTOR_SIZE);
        if (physical == PhysicalType::VARCHAR) {
            // For VARCHAR, copy value by value.
            for (idx_t i = 0; i < rows_to_append; i++) {
                temp.SetValue(i, src_vec.GetValue(offset + i));
            }
        } else if (type_size > 0) {
            // For fixed-size types, memcpy the slice.
            std::memcpy(temp.GetData(), src_vec.GetData() + offset * type_size,
                        rows_to_append * type_size);
            // Copy validity.
            for (idx_t i = 0; i < rows_to_append; i++) {
                if (!src_vec.GetValidity().RowIsValid(offset + i)) {
                    temp.GetValidity().SetInvalid(i);
                }
            }
        }

        columns_[col]->Append(temp, rows_to_append);
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

#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/vector.hpp"
#include "slothdb/common/constants.hpp"
#include <memory>
#include <vector>

namespace slothdb {

// Stores a single column's data within a RowGroup.
// Data is stored as a contiguous buffer of the physical type,
// plus a ValidityMask for nulls.
class ColumnData {
public:
    ColumnData(const LogicalType &type, idx_t capacity);

    const LogicalType &GetType() const { return type_; }
    idx_t Count() const { return count_; }

    // Append values from a vector into this column.
    void Append(Vector &source, idx_t source_count);

    // Read values into a target vector. Reads from [start, start + count).
    void Scan(Vector &target, idx_t start, idx_t scan_count) const;

private:
    LogicalType type_;
    idx_t capacity_;
    idx_t count_;

    // Raw data storage.
    std::unique_ptr<data_t[]> data_;
    ValidityMask validity_;

    // For VARCHAR: owns the string data.
    std::vector<std::string> string_heap_;

    idx_t TypeSize() const;
};

} // namespace slothdb

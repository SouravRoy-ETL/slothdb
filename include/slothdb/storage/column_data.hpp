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

    // Append values from a vector range [offset, offset+count). For VARCHAR
    // columns this is a zero-copy fast path: the source's string buffer is
    // shared via shared_ptr rather than copying each string. Used by callers
    // (like bulk loaders) that want to avoid the per-cell std::string
    // allocation of the generic Append path.
    void AppendSlice(Vector &source, idx_t offset, idx_t count);

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
    // For zero-copy VARCHAR append - keeps source VectorStringBuffers alive so
    // string_t pointers in `data_` stay valid.
    std::vector<std::shared_ptr<VectorBuffer>> held_bufs_;

    idx_t TypeSize() const;
};

} // namespace slothdb

#pragma once

#include "slothdb/common/types/vector.hpp"
#include <vector>

namespace slothdb {

// A collection of Vectors (one per column), all with the same row count.
// This is the unit of data flow through the execution engine.
class DataChunk {
public:
    DataChunk() : count_(0) {}
    ~DataChunk() = default;

    // No copy, allow move.
    DataChunk(const DataChunk &) = delete;
    DataChunk &operator=(const DataChunk &) = delete;
    DataChunk(DataChunk &&) = default;
    DataChunk &operator=(DataChunk &&) = default;

    // Initialize with column types.
    void Initialize(const std::vector<LogicalType> &types, idx_t capacity = VECTOR_SIZE);

    // Number of columns.
    idx_t ColumnCount() const { return static_cast<idx_t>(data.size()); }

    // Number of rows.
    idx_t size() const { return count_; }

    void SetCardinality(idx_t count) { count_ = count; }
    void SetCardinality(const DataChunk &other) { count_ = other.count_; }

    // Reset to empty (0 rows), keeping column structure.
    void Reset();

    // Access a column vector.
    Vector &GetVector(idx_t col_idx) { return data[col_idx]; }
    const Vector &GetVector(idx_t col_idx) const { return data[col_idx]; }

    // Get/set individual values (slow path).
    Value GetValue(idx_t col_idx, idx_t row_idx) const;
    void SetValue(idx_t col_idx, idx_t row_idx, const Value &val);

    // Append rows from another chunk.
    void Append(const DataChunk &other);

    // Debug printing.
    std::string ToString() const;

    // Column vectors (public for direct access in operators).
    std::vector<Vector> data;

private:
    idx_t count_;
};

} // namespace slothdb

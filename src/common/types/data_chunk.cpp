#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/exception.hpp"
#include <sstream>

namespace slothdb {

void DataChunk::Initialize(const std::vector<LogicalType> &types, idx_t capacity) {
    data.clear();
    data.reserve(types.size());
    for (auto &type : types) {
        data.emplace_back(type, capacity);
    }
    count_ = 0;
}

void DataChunk::Reset() {
    count_ = 0;
    for (auto &vec : data) {
        vec.GetValidity().Reset();
    }
}

Value DataChunk::GetValue(idx_t col_idx, idx_t row_idx) const {
    return data[col_idx].GetValue(row_idx);
}

void DataChunk::SetValue(idx_t col_idx, idx_t row_idx, const Value &val) {
    data[col_idx].SetValue(row_idx, val);
    if (row_idx >= count_) {
        count_ = row_idx + 1;
    }
}

void DataChunk::Append(const DataChunk &other) {
    if (ColumnCount() != other.ColumnCount()) {
        throw InternalException("DataChunk::Append column count mismatch");
    }
    idx_t new_count = count_ + other.size();
    if (new_count > VECTOR_SIZE) {
        throw InternalException("DataChunk::Append would exceed VECTOR_SIZE");
    }
    for (idx_t col = 0; col < ColumnCount(); col++) {
        for (idx_t row = 0; row < other.size(); row++) {
            data[col].SetValue(count_ + row, other.data[col].GetValue(row));
        }
    }
    count_ = new_count;
}

std::string DataChunk::ToString() const {
    std::ostringstream ss;
    for (idx_t row = 0; row < count_; row++) {
        for (idx_t col = 0; col < ColumnCount(); col++) {
            if (col > 0) ss << "\t";
            ss << GetValue(col, row).ToString();
        }
        ss << "\n";
    }
    return ss.str();
}

} // namespace slothdb

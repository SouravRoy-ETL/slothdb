#include "slothdb/execution/literal_emit_filter.hpp"

#include <algorithm>

#include "slothdb/common/constants.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/vector.hpp"

namespace slothdb {

PhysicalLiteralEmitFilter::PhysicalLiteralEmitFilter(
    std::vector<LogicalType> types, Value literal,
    std::unique_ptr<PhysicalOperator> counter)
    : PhysicalOperator(PhysicalOperatorType::PROJECTION, std::move(types)),
      literal_(std::move(literal)), counter_(std::move(counter)) {}

void PhysicalLiteralEmitFilter::SetNeededOutputs(const std::vector<bool> &) {
    std::vector<bool> count_mask(1, true);
    counter_->SetNeededOutputs(count_mask);
}

void PhysicalLiteralEmitFilter::Init() {
    counter_->Init();
    DataChunk chunk;
    chunk.Initialize(counter_->GetTypes());
    match_count_ = 0;
    while (counter_->GetData(chunk)) {
        if (chunk.size() > 0 && chunk.ColumnCount() > 0) {
            auto v = chunk.GetValue(0, 0);
            if (!v.IsNull()) {
                match_count_ = v.GetValue<int64_t>();
            }
        }
        chunk.Reset();
    }
    emitted_ = 0;
}

bool PhysicalLiteralEmitFilter::GetData(DataChunk &result) {
    if (emitted_ >= match_count_) return false;
    if (result.ColumnCount() != GetTypes().size()) result.Initialize(GetTypes());
    else result.Reset();
    idx_t remaining = static_cast<idx_t>(match_count_ - emitted_);
    idx_t count = std::min<idx_t>(remaining, VECTOR_SIZE);
    auto &vec = result.GetVector(0);
    auto tid = vec.GetType().id();
    if (tid == LogicalTypeId::BIGINT) {
        int64_t lv = literal_.GetValue<int64_t>();
        auto *d = vec.GetData<int64_t>();
        for (idx_t i = 0; i < count; i++) d[i] = lv;
    } else if (tid == LogicalTypeId::INTEGER) {
        int32_t lv = (int32_t)literal_.GetValue<int64_t>();
        auto *d = vec.GetData<int32_t>();
        for (idx_t i = 0; i < count; i++) d[i] = lv;
    } else {
        for (idx_t i = 0; i < count; i++) vec.SetValue(i, literal_);
    }
    result.SetCardinality(count);
    emitted_ += count;
    return true;
}

}  // namespace slothdb

#pragma once

// PhysicalLiteralEmitFilter — kept in a side TU per
// feedback_text_icache_shift.md: any new code added inline to
// physical_planner.cpp risks shifting .text and regressing unrelated
// hot queries (Q21/Q28 demonstrated).

#include "slothdb/common/types/value.hpp"
#include "slothdb/execution/physical_operator.hpp"

namespace slothdb {

class PhysicalLiteralEmitFilter : public PhysicalOperator {
public:
    PhysicalLiteralEmitFilter(std::vector<LogicalType> types, Value literal,
                               std::unique_ptr<PhysicalOperator> counter);
    void SetNeededOutputs(const std::vector<bool> &) override;
    void Init() override;
    bool GetData(DataChunk &result) override;

private:
    Value literal_;
    std::unique_ptr<PhysicalOperator> counter_;
    int64_t match_count_ = 0;
    int64_t emitted_ = 0;
};

}  // namespace slothdb

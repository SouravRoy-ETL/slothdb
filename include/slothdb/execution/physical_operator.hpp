#pragma once

#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include <memory>
#include <vector>

namespace slothdb {

enum class PhysicalOperatorType : uint8_t {
    TABLE_SCAN,
    FILTER,
    PROJECTION,
    ORDER_BY,
    LIMIT,
    INSERT,
    CREATE_TABLE,
    DROP_TABLE,
    DUMMY_SCAN
};

// Base class for physical operators. Pull-based: call GetData() to get next chunk.
class PhysicalOperator {
public:
    PhysicalOperator(PhysicalOperatorType type, std::vector<LogicalType> types)
        : type_(type), types_(std::move(types)) {}
    virtual ~PhysicalOperator() = default;

    PhysicalOperatorType GetOperatorType() const { return type_; }
    const std::vector<LogicalType> &GetTypes() const { return types_; }

    // Initialize the operator for execution.
    virtual void Init() {}

    // Get the next chunk of data. Returns false when done.
    virtual bool GetData(DataChunk &result) = 0;

    std::vector<std::unique_ptr<PhysicalOperator>> children;

private:
    PhysicalOperatorType type_;
    std::vector<LogicalType> types_;
};

using PhysicalOpPtr = std::unique_ptr<PhysicalOperator>;

} // namespace slothdb

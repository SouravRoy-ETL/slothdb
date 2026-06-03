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

    // DML affected-row count. -1 = not a DML statement (no count). INSERT/
    // UPDATE/DELETE operators override this so Connection::Query can return
    // the affected-row count as a single-row BIGINT result (matches DuckDB/
    // DataFusion, which return [(count,)] for INSERT/UPDATE/DELETE).
    virtual int64_t AffectedRows() const { return -1; }

    // Projection pushdown: mark which output columns consumers need.
    // Operators may use this to skip writing unneeded columns.
    virtual void SetNeededOutputs(const std::vector<bool> &) {}

    // LIMIT pushdown: hint how many rows the consumer will actually read.
    // Sort-heavy operators can use this for partial_sort / nth_element.
    virtual void SetRowLimit(idx_t) {}

    // TopN pushdown: when an OrderBy(simple_col_ref) feeds a Limit(K), the
    // OrderBy can tell its child "I only need the top-K rows by output column
    // `col_idx`". Aggregates can then keep a bounded heap during emit instead
    // of materializing all rows. `col_idx` is in this operator's OUTPUT slot
    // space; intermediates (e.g. Projection) translate before forwarding.
    virtual void SetTopNHint(idx_t /*col_idx*/, bool /*ascending*/,
                              idx_t /*limit*/) {}

    std::vector<std::unique_ptr<PhysicalOperator>> children;

private:
    PhysicalOperatorType type_;
    std::vector<LogicalType> types_;
};

using PhysicalOpPtr = std::unique_ptr<PhysicalOperator>;

} // namespace slothdb

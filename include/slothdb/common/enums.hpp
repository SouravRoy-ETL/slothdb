#pragma once

#include <cstdint>
#include "slothdb/common/constants.hpp"

namespace slothdb {

// PhysicalType represents the actual C++ storage type used in vectors.
// Multiple LogicalTypes can map to the same PhysicalType.
enum class PhysicalType : uint8_t {
    BOOL,
    INT8,
    INT16,
    INT32,
    INT64,
    INT128,
    UINT8,
    UINT16,
    UINT32,
    UINT64,
    FLOAT,
    DOUBLE,
    VARCHAR,
    INTERVAL,
    STRUCT,
    LIST,
    ARRAY,
    INVALID
};

// Returns the byte size of a fixed-size physical type.
// Returns 0 for variable-size types (VARCHAR, STRUCT, LIST, ARRAY).
idx_t GetTypeIdSize(PhysicalType type);

} // namespace slothdb

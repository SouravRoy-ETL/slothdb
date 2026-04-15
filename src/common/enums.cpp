#include "slothdb/common/enums.hpp"
#include <stdexcept>

namespace slothdb {

idx_t GetTypeIdSize(PhysicalType type) {
    switch (type) {
    case PhysicalType::BOOL:
    case PhysicalType::INT8:
    case PhysicalType::UINT8:
        return 1;
    case PhysicalType::INT16:
    case PhysicalType::UINT16:
        return 2;
    case PhysicalType::INT32:
    case PhysicalType::UINT32:
    case PhysicalType::FLOAT:
        return 4;
    case PhysicalType::INT64:
    case PhysicalType::UINT64:
    case PhysicalType::DOUBLE:
        return 8;
    case PhysicalType::INT128:
        return 16;
    case PhysicalType::VARCHAR:
        return 16; // string_t is 16 bytes
    case PhysicalType::INTERVAL:
        return 16;
    case PhysicalType::STRUCT:
    case PhysicalType::LIST:
    case PhysicalType::ARRAY:
        return 0; // variable-size, handled separately
    case PhysicalType::INVALID:
        return 0;
    }
    return 0;
}

} // namespace slothdb

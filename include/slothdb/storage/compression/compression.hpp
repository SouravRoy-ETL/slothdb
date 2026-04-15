#pragma once

#include "slothdb/common/constants.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

enum class CompressionType : uint8_t {
    UNCOMPRESSED = 0,
    CONSTANT = 1,       // All values identical.
    RLE = 2,            // Run-length encoding.
    DICTIONARY = 3,     // Dictionary encoding (for low-cardinality).
    BITPACKING = 4,     // Pack integers into minimum bits.
};

const char *CompressionTypeToString(CompressionType type);

// Zone map: min/max statistics for a column segment.
struct ZoneMap {
    Value min_value;
    Value max_value;
    idx_t count = 0;
    idx_t null_count = 0;
    bool has_stats = false;

    // Check if a predicate can skip this segment.
    // Returns true if the segment MIGHT contain matching rows.
    bool MightContain(const std::string &op, const Value &val) const;
};

// Compressed data buffer.
struct CompressedBuffer {
    CompressionType type = CompressionType::UNCOMPRESSED;
    std::vector<uint8_t> data;
    idx_t count = 0;          // Number of values.
    ZoneMap zone_map;
};

// Compression codec interface.
class CompressionCodec {
public:
    virtual ~CompressionCodec() = default;

    // Compress a column of values. Returns compressed buffer.
    virtual CompressedBuffer Compress(const std::vector<Value> &values,
                                       const LogicalType &type) = 0;

    // Decompress back to values.
    virtual std::vector<Value> Decompress(const CompressedBuffer &buffer,
                                           const LogicalType &type) = 0;

    // Get the compression type.
    virtual CompressionType GetType() const = 0;
};

// Factory: get all available codecs and auto-select best.
class CompressionManager {
public:
    // Auto-select the best compression for this data.
    static CompressedBuffer Compress(const std::vector<Value> &values,
                                      const LogicalType &type);

    // Decompress using the codec indicated by the buffer's type.
    static std::vector<Value> Decompress(const CompressedBuffer &buffer,
                                          const LogicalType &type);

    // Compute zone map for a set of values.
    static ZoneMap ComputeZoneMap(const std::vector<Value> &values);
};

} // namespace slothdb

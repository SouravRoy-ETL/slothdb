#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <string>
#include <vector>

namespace slothdb {

// Avro file reader.
// Avro format: [magic "Obj\x01"][file header (JSON schema + codec + sync marker)]
//              [data blocks]*
// Each data block: [object_count(varint)][block_size(varint)][encoded_data][sync_marker(16)]
// Simplified: reads Avro files with "null" codec (no compression) and flat record schemas.

class AvroReader {
public:
    explicit AvroReader(const std::string &path);

    const std::vector<std::string> &GetColumnNames() const { return column_names_; }
    const std::vector<LogicalType> &GetColumnTypes() const { return column_types_; }
    int64_t NumRows() const { return static_cast<int64_t>(rows_.size()); }

    std::vector<std::vector<Value>> ReadAll();

private:
    void Parse();
    int64_t ReadVarInt(const uint8_t *data, size_t &pos, size_t size);
    std::string ReadAvroString(const uint8_t *data, size_t &pos, size_t size);

    std::string path_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    std::vector<std::string> avro_types_;  // Avro type names.
    std::vector<std::vector<Value>> rows_;
    bool parsed_ = false;
};

} // namespace slothdb

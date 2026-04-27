#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include "slothdb/common/types/data_chunk.hpp"
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

    // Populate `column_names_` / `column_types_` by parsing only the file
    // header (magic + metadata map + sync marker). No data-block parse.
    // Used by PhysicalAvroScan so the planner can set up the catalog
    // entry without loading a single row of data.
    void DetectSchemaLight();

    // Stream parse directly into `chunks` - skips the per-cell Value boxing
    // that goes through `rows_`. Each field writes directly into its
    // column Vector (typed memcpy for numerics, string_t for VARCHAR).
    // Used by PhysicalAvroScan::Init().
    void ReadIntoChunks(std::vector<DataChunk> &chunks,
                        const std::vector<LogicalType> &types);

private:
    void Parse();
    int64_t ReadVarInt(const uint8_t *data, size_t &pos, size_t size);
    std::string ReadAvroString(const uint8_t *data, size_t &pos, size_t size);

    std::string path_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    std::vector<std::string> avro_types_;  // Avro primitive type names.
    // Per-column nullable flag — true when the schema field is a
    // ["null", T] (or [T, "null"]) union. The wire format for these
    // fields prefixes every value with a 1-byte union index (0=null,
    // 1=value). Skipping that byte was the cause of issue #5.
    std::vector<uint8_t> avro_nullable_;
    // Per-column scale for logical types. 0 = no logical type.
    // 1000 = Avro timestamp-millis (multiply long-millis by 1000 to
    // convert to SlothDB's microsecond TIMESTAMP).
    std::vector<int64_t> avro_ts_scale_;
    std::vector<std::vector<Value>> rows_;
    bool parsed_ = false;

    // Helper: extract avro_type / nullable / ts_scale + logical-type
    // shaped column type from a single field's schema fragment.
    void ParseFieldSchema(const std::string &field_schema,
                          std::string &out_avro_type,
                          bool &out_nullable,
                          int64_t &out_ts_scale,
                          LogicalType &out_logical_type);
};

} // namespace slothdb

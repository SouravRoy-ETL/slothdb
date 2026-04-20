#pragma once

#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <string>
#include <vector>

namespace slothdb {

// Arrow IPC (Feather v2) reader/writer.
// Simplified implementation for flat (non-nested) columnar data.
// Format: [ARROW1 magic][schema][record batches][footer][footer_size][ARROW1]

class ArrowIPCWriter {
public:
    ArrowIPCWriter(const std::string &path,
                   const std::vector<std::string> &column_names,
                   const std::vector<LogicalType> &column_types);

    void WriteBatch(const std::vector<std::vector<Value>> &rows);
    void Finish();

private:
    std::string path_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    std::vector<std::vector<std::vector<Value>>> batches_;
};

class ArrowIPCReader {
public:
    explicit ArrowIPCReader(const std::string &path);

    const std::vector<std::string> &GetColumnNames() const { return column_names_; }
    const std::vector<LogicalType> &GetColumnTypes() const { return column_types_; }
    int64_t NumRows() const;

    std::vector<std::vector<Value>> ReadAll();

    // Parse only the magic + schema, skipping the row data. Populates
    // column_names_/column_types_ so callers can build a catalog entry
    // without reading the whole file.
    void DetectSchemaLight();

    // Stream-parse the file body directly into typed DataChunk vectors,
    // mirroring JSONReader / AvroReader. Skips the Value-boxed rows_
    // intermediate, so we don't pay for per-cell std::string allocation
    // when the DataChunk vectors already have typed storage.
    void ReadIntoChunks(std::vector<DataChunk> &chunks,
                        const std::vector<LogicalType> &types);

private:
    std::string path_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    std::vector<std::vector<Value>> rows_;
    bool parsed_ = false;
    bool schema_parsed_ = false;
    void Parse();
};

} // namespace slothdb

#pragma once

#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include "slothdb/common/constants.hpp"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace slothdb {

// Parquet physical types (from spec).
enum class ParquetType : int32_t {
    BOOLEAN = 0,
    INT32 = 1,
    INT64 = 2,
    FLOAT = 4,
    DOUBLE = 5,
    BYTE_ARRAY = 6
};

// Simplified Parquet metadata structures.
struct ParquetColumnMeta {
    std::string name;
    ParquetType parquet_type;
    LogicalType slothdb_type;
    int64_t num_values = 0;
    int64_t data_offset = 0;
    int64_t data_size = 0;
    // Statistics for predicate pushdown.
    bool has_stats = false;
    Value min_value;
    Value max_value;
};

struct ParquetRowGroup {
    int64_t num_rows = 0;
    std::vector<ParquetColumnMeta> columns;
};

struct ParquetFileMeta {
    int64_t num_rows = 0;
    std::vector<ParquetRowGroup> row_groups;
    std::vector<std::string> column_names;
    std::vector<LogicalType> column_types;
};

// ============================================================================
// Parquet Writer
// ============================================================================

class ParquetWriter {
public:
    ParquetWriter(const std::string &path,
                  const std::vector<std::string> &column_names,
                  const std::vector<LogicalType> &column_types);
    ~ParquetWriter();

    void WriteRowGroup(const std::vector<std::vector<Value>> &rows);
    void Finish();

private:
    void WriteColumnChunk(const std::vector<Value> &values, const LogicalType &type,
                          ParquetColumnMeta &meta);
    void WriteThriftMeta();
    // Thrift compact protocol helpers.
    void WriteThriftFieldI32(std::vector<uint8_t> &buf, int field_id, int32_t val);
    void WriteThriftFieldI64(std::vector<uint8_t> &buf, int field_id, int64_t val);
    void WriteThriftFieldString(std::vector<uint8_t> &buf, int field_id, const std::string &val);
    void WriteThriftFieldList(std::vector<uint8_t> &buf, int field_id, int elem_type, int count);
    void WriteThriftFieldStruct(std::vector<uint8_t> &buf, int field_id);
    void WriteThriftStop(std::vector<uint8_t> &buf);
    void WriteVarInt(std::vector<uint8_t> &buf, uint64_t val);

    std::ofstream file_;
    std::string path_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    ParquetFileMeta meta_;
    int64_t current_offset_ = 4; // After PAR1 magic.
    bool finished_ = false;
};

// ============================================================================
// Parquet Reader
// ============================================================================

class ParquetReader {
public:
    explicit ParquetReader(const std::string &path);

    const ParquetFileMeta &GetMeta() const { return meta_; }
    const std::vector<std::string> &GetColumnNames() const { return meta_.column_names; }
    const std::vector<LogicalType> &GetColumnTypes() const { return meta_.column_types; }
    int64_t NumRows() const { return meta_.num_rows; }

    // Read all rows.
    std::vector<std::vector<Value>> ReadAll();

    // Read a specific row group (for predicate pushdown).
    std::vector<std::vector<Value>> ReadRowGroup(idx_t rg_idx);

    // Streaming: read one row group directly into a DataChunk.
    // If projection is non-empty, only loads columns where projection[col]==true.
    // Returns rows read; chunk is filled.
    idx_t ReadRowGroupChunk(idx_t rg_idx, DataChunk &chunk,
                             const std::vector<bool> &projection = {});

    idx_t NumRowGroups() const { return static_cast<idx_t>(meta_.row_groups.size()); }

    // Check if a row group might contain rows matching a predicate.
    bool RowGroupMightMatch(idx_t rg_idx, idx_t col_idx,
                            const std::string &op, const Value &val) const;

private:
    void ReadMetadata();
    std::vector<Value> ReadColumnChunk(const ParquetColumnMeta &meta);
    // Thrift parsing helpers.
    int32_t ReadThriftVarInt(const uint8_t *data, size_t &pos);
    int64_t ReadThriftVarInt64(const uint8_t *data, size_t &pos);

    std::string path_;
    ParquetFileMeta meta_;
    bool meta_read_ = false;
};

} // namespace slothdb

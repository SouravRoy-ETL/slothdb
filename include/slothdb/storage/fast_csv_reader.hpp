#pragma once

#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include "slothdb/common/constants.hpp"
#include <string>
#include <vector>
#include <cstdio>

namespace slothdb {

class DataTable;

// High-performance CSV reader that reads entire file into memory buffer
// and parses directly into DataChunks without intermediate allocations.
class FastCSVReader {
public:
    // Full load — reads entire file into memory.
    FastCSVReader(const std::string &path, char delimiter = ',', bool has_header = true);
    // Sample-only load — reads up to sample_bytes for schema detection.
    FastCSVReader(const std::string &path, char delimiter, bool has_header, size_t sample_bytes);
    // Borrowed buffer range — does not own the buffer.
    FastCSVReader(const char *buffer, size_t start, size_t end, char delimiter);
    ~FastCSVReader();

    // Read header row.
    std::vector<std::string> ReadHeader();

    // Detect types from first N rows.
    std::vector<LogicalType> DetectTypes(idx_t sample_size = 100);

    // Stream all data directly into a DataTable.
    void ReadIntoTable(DataTable &table, const std::vector<LogicalType> &types);

    // Read one chunk (up to VECTOR_SIZE rows) directly into DataChunk.
    idx_t ReadChunk(DataChunk &chunk, const std::vector<LogicalType> &types);

    // Same as ReadChunk but only parses values for columns where projection[col]==true.
    // Other columns get NULL but delimiters are still scanned (for line boundaries).
    idx_t ReadChunkProjected(DataChunk &chunk, const std::vector<LogicalType> &types,
                              const std::vector<bool> &projection);

    // Count remaining rows without parsing fields (fast path for COUNT(*)).
    idx_t CountRows();

    // Expose internal buffer for parallel processing.
    const char *GetBuffer() const { return buffer_; }
    size_t GetSize() const { return size_; }
    size_t GetPos() const { return pos_; }
    void SetPos(size_t p) { pos_ = p; }

    // Find next newline at or after offset, return offset of byte after newline.
    static size_t FindLineStart(const char *buf, size_t size, size_t offset);

    bool IsEOF() const { return pos_ >= size_; }

private:
    // Skip to next line.
    void SkipLine();

    // Parse next field in-place, return start and length in buffer.
    // Does NOT allocate strings for numeric fields.
    bool NextField(const char *&field_start, size_t &field_len);

    // Fast numeric conversion without string allocation.
    static int64_t ParseInt64(const char *s, size_t len);
    static double ParseDouble(const char *s, size_t len);

    char *buffer_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    char delimiter_;
    bool has_header_;
    bool header_read_ = false;
    idx_t num_fields_ = 0;
    bool owns_buffer_ = true;
    void *mmap_handle_ = nullptr;
};

} // namespace slothdb

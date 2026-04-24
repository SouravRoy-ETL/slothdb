#pragma once

#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include "slothdb/common/constants.hpp"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace slothdb {

class DataTable;

// High-performance CSV reader that reads entire file into memory buffer
// and parses directly into DataChunks without intermediate allocations.
class FastCSVReader {
public:
    // Full load - reads entire file into memory.
    FastCSVReader(const std::string &path, char delimiter = ',', bool has_header = true);
    // Sample-only load - reads up to sample_bytes for schema detection.
    FastCSVReader(const std::string &path, char delimiter, bool has_header, size_t sample_bytes);
    // Borrowed buffer range - does not own the buffer.
    FastCSVReader(const char *buffer, size_t start, size_t end, char delimiter);
    ~FastCSVReader();

    // Read header row.
    std::vector<std::string> ReadHeader();

    // Detect types from first N rows.
    std::vector<LogicalType> DetectTypes(idx_t sample_size = 100);

    // Stream all data directly into a DataTable.
    void ReadIntoTable(DataTable &table, const std::vector<LogicalType> &types);

    // Parallel-parse the remainder of the buffer into a vector of DataChunks.
    // Splits the buffer into N line-aligned ranges (N = min(hardware_concurrency, 8)),
    // each worker parses its range via a borrowed-buffer FastCSVReader, chunks
    // are appended in file order after join. `projection` optionally restricts
    // which columns get materialized (same semantics as ReadChunkProjected).
    void ReadIntoChunks(std::vector<DataChunk> &out,
                        const std::vector<LogicalType> &types,
                        const std::vector<bool> *projection = nullptr);

    // Read one chunk (up to VECTOR_SIZE rows) directly into DataChunk.
    idx_t ReadChunk(DataChunk &chunk, const std::vector<LogicalType> &types);

    // Same as ReadChunk but only parses values for columns where projection[col]==true.
    // Other columns get NULL but delimiters are still scanned (for line boundaries).
    idx_t ReadChunkProjected(DataChunk &chunk, const std::vector<LogicalType> &types,
                              const std::vector<bool> &projection);

    // Ultra-fast GROUP BY COUNT(*) on a single VARCHAR column. Takes a callback
    // that receives (data, len) per row. Bypasses vector writes entirely.
    template<class Fn>
    idx_t ForEachVarcharCol(idx_t col_idx, idx_t num_cols, Fn &&fn);

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

template<class Fn>
idx_t FastCSVReader::ForEachVarcharCol(idx_t col_idx, idx_t num_cols, Fn &&fn) {
    if (has_header_ && !header_read_) ReadHeader();
    (void)num_cols;
    idx_t count = 0;
    const char delim = delimiter_;

    auto scan_to_delim = [&](const char *p) -> const char * {
#if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        const char *end = buffer_ + size_;
        __m128i vd = _mm_set1_epi8(delim);
        __m128i vn = _mm_set1_epi8('\n');
        while (p + 16 <= end) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
            __m128i cmp = _mm_or_si128(_mm_cmpeq_epi8(chunk, vd), _mm_cmpeq_epi8(chunk, vn));
            int mask = _mm_movemask_epi8(cmp);
            if (mask) {
#ifdef _MSC_VER
                unsigned long idx; _BitScanForward(&idx, (unsigned long)mask);
                return p + idx;
#else
                return p + __builtin_ctz(mask);
#endif
            }
            p += 16;
        }
        while (p < end && *p != delim && *p != '\n') p++;
        return p;
#else
        const char *end = buffer_ + size_;
        while (p < end && *p != delim && *p != '\n') p++;
        return p;
#endif
    };

    while (pos_ < size_) {
        for (idx_t c = 0; c < col_idx; c++) {
            const char *d = scan_to_delim(buffer_ + pos_);
            if (d == buffer_ + size_ || *d != delim) { pos_ = size_; break; }
            pos_ = (size_t)(d - buffer_ + 1);
        }
        if (pos_ >= size_) break;
        const char *field_start = buffer_ + pos_;
        const char *field_end = scan_to_delim(field_start);
        size_t field_len = field_end - field_start;
        fn(field_start, field_len);
        pos_ = (size_t)(field_end - buffer_);
        while (pos_ < size_ && buffer_[pos_] != '\n') pos_++;
        if (pos_ < size_) pos_++;
        count++;
    }
    return count;
}

} // namespace slothdb

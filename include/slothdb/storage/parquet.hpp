#pragma once

#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/string_type.hpp"
#include "slothdb/common/types/value.hpp"
#include "slothdb/common/constants.hpp"
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdlib>

namespace slothdb {

// Decode profiling (SLOTH_PROFILE=1). Nanosecond accumulators summed across
// parquet worker threads. Diagnostic only - one predicted-false branch per
// page/call when disabled.
struct PqDecodeProfile {
    std::atomic<uint64_t> decomp_ns{0};      // codec decompression (Snappy/etc)
    std::atomic<uint64_t> pagedecode_ns{0};  // DecodeDataPageTyped (def+RLE+gather+writes)
    std::atomic<uint64_t> rgdecode_ns{0};    // whole DecodeRowGroupInto (incl dict parse + headers)
    std::atomic<uint64_t> consume_ns{0};     // RGConsumer callback (the aggregation)
    std::atomic<uint64_t> agg_pass1_ns{0};   // agg: per-row histogram pass
    std::atomic<uint64_t> agg_pass2_ns{0};   // agg: per-dict-entry fold-into-map pass
    std::atomic<uint64_t> agg_dict_sum{0};   // agg: sum of dict_size across RG calls
    std::atomic<uint64_t> npages{0};
    std::atomic<uint64_t> c_skipdi{0};       // consumer RGs taking SkipDi branch
    std::atomic<uint64_t> c_dictrg{0};       // consumer RGs taking IncrementByDictRG branch
    std::atomic<uint64_t> c_perrow{0};       // consumer RGs taking per-row IncrementRow branch
    void Reset() { decomp_ns = 0; pagedecode_ns = 0; rgdecode_ns = 0; consume_ns = 0;
                   agg_pass1_ns = 0; agg_pass2_ns = 0; agg_dict_sum = 0; npages = 0;
                   c_skipdi = 0; c_dictrg = 0; c_perrow = 0; }
};
inline PqDecodeProfile g_pq_profile;
inline bool PqProfileOn() {
    static const bool on = [] {
        return std::getenv("SLOTH_PROFILE") != nullptr;
    }();
    return on;
}

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
    // Standard-Parquet fields (populated when reading real Parquet files).
    int32_t codec = 0; // 0=UNCOMPRESSED, 1=SNAPPY, 2=GZIP, ...
    int64_t total_uncompressed_size = 0;
    int64_t total_compressed_size = 0;
    int64_t dict_page_offset = -1; // -1 if column has no dictionary page
    // Repetition type from the schema (flat schemas only):
    // 0 = REQUIRED  (max_def_level = 0, no def_levels in data pages)
    // 1 = OPTIONAL  (max_def_level = 1, single-bit def_levels)
    // 2 = REPEATED  (not currently supported by the typed-decode path)
    int32_t repetition_type = 1; // default OPTIONAL for backwards compat
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
    // Per-leaf repetition type from the schema:
    // 0=REQUIRED (max_def_level=0), 1=OPTIONAL (max_def_level=1), 2=REPEATED.
    // Only populated for files parsed via the standard-Parquet thrift path.
    std::vector<int32_t> column_repetition;
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
// Typed column data for a full row group - avoids boxing every value in Value.
// Only one of (int32/int64/float/double/bool data) is populated depending on
// the LogicalType.  VARCHAR uses str_data + str_heap (shared so the Vector
// can keep the backing bytes alive for its own lifetime).
// ============================================================================
struct ParquetColumnData {
    LogicalType type = LogicalType::VARCHAR();
    idx_t count = 0;
    // Typed raw data (one of these is used; selected by type).
    std::vector<int32_t> i32_data;
    std::vector<int64_t> i64_data;
    std::vector<float> f32_data;
    std::vector<double> f64_data;
    std::vector<uint8_t> bool_data;
    // VARCHAR: fixed-size string_t entries pointing into str_heap (or inline).
    std::vector<string_t> str_data;
    std::shared_ptr<std::vector<char>> str_heap;
    // VARCHAR dict-encoded fast path: per-row dict index + the dict itself.
    // Populated only when the column chunk was entirely dict-encoded; lets
    // consumers (e.g. GROUP BY) do O(1) array-index lookup per row and avoid
    // per-row string comparisons.
    std::vector<uint32_t> str_dict_indices;
    std::vector<string_t> str_dict_values;
    bool str_dict_encoded = false;
    // When true, the decoder skipped writing str_data[r] - consumers must
    // resolve strings via str_dict_values[str_dict_indices[r]] instead.
    bool str_data_skipped = false;
    // One byte per row: 0 = null, 1 = valid. Empty means all_valid = true.
    std::vector<uint8_t> validity;
    bool all_valid = true;
    // Set true if native decode succeeded; false falls back to Value path.
    bool decoded = false;
    // Lengths-only fast path: when consumers only need string LENGTH (e.g.
    // `WHERE col <> ''` length-check, `STRLEN(col)` aggregate arg), the
    // decoder skips byte materialization entirely and fills only `str_lengths`.
    // Placed at end of struct to preserve cache-line layout of pre-existing
    // fields (per memory feedback_struct_growth_cache_shifts.md).
    std::vector<uint32_t> str_lengths;
    bool str_lengths_only = false;
    // Dict-only fast path: decode only the dict page; skip all data pages.
    // Output: str_dict_values populated, str_dict_indices stays empty, no
    // str_data. Consumers that only need to enumerate the dict (e.g. Q26
    // ORDER BY col LIMIT N with no orphan-check) set this to skip ~hundreds
    // of MB of RLE+snappy decode for the data pages.
    bool str_dict_only = false;
    // Dict-used fast path: decode dict + data pages, but only mark which dict
    // entries are referenced. Skips str_dict_indices buffer materialization
    // (~25MB/RG write) AND the consumer's O(N) used[] scan. The decoder fills
    // str_dict_used bitmap (size = dict_size) directly from the RLE-decoded
    // batch buffer. Used by Q26 (ORDER BY col LIMIT N) when an orphan-safe
    // dict-trust path is needed.
    std::vector<uint8_t> str_dict_used;
    bool str_dict_used_only = false;

    void Clear() {
        count = 0;
        i32_data.clear(); i64_data.clear();
        f32_data.clear(); f64_data.clear();
        bool_data.clear(); str_data.clear();
        str_heap.reset();
        str_dict_indices.clear();
        str_dict_values.clear();
        str_dict_encoded = false;
        validity.clear();
        all_valid = true;
        decoded = false;
        str_lengths.clear();
        str_lengths_only = false;
        str_dict_only = false;
        str_dict_used.clear();
        str_dict_used_only = false;
    }
};

// ============================================================================
// Parquet Reader
// ============================================================================

class ParquetReader {
public:
    explicit ParquetReader(const std::string &path);
    ~ParquetReader();
    ParquetReader(const ParquetReader &) = delete;
    ParquetReader &operator=(const ParquetReader &) = delete;

    const ParquetFileMeta &GetMeta() const { return meta_; }
    const std::vector<std::string> &GetColumnNames() const { return meta_.column_names; }
    const std::vector<LogicalType> &GetColumnTypes() const { return meta_.column_types; }
    int64_t NumRows() const { return meta_.num_rows; }

    // Read all rows.
    std::vector<std::vector<Value>> ReadAll();

    // Read a specific row group (for predicate pushdown).
    std::vector<std::vector<Value>> ReadRowGroup(idx_t rg_idx);

    // Read a single column of a row group. Used by projection-aware scans.
    std::vector<Value> ReadColumn(idx_t rg_idx, idx_t col_idx);

    // Native typed decode: fills `out` with the column's values decoded
    // directly into typed buffers (no Value boxing). Returns true on success
    // - on false, caller should use ReadColumn as a fallback. Currently
    // handles BOOLEAN/INT32/INT64/FLOAT/DOUBLE/VARCHAR for standard Parquet
    // files with PLAIN / PLAIN_DICTIONARY / RLE_DICTIONARY encodings.
    //
    // `skip_str_data`: for VARCHAR dict-encoded columns, skip writing the
    // per-row `str_data[r] = string_t(...)` (populate only `str_dict_indices`
    // + `str_dict_values`). Saves ~160 MB of string_t writes on a 10M-row
    // dict-encoded VARCHAR column. The caller is responsible for reading
    // `str_dict_values[str_dict_indices[r]]` to get the string value.
    //
    // `filter_mask`: optional per-row keep mask (size == column rows). When
    // non-null, the PLAIN VARCHAR decoder skips dst[i] writes for masked-
    // out rows (Q22-shape selection-vector pushdown). The downstream
    // consumer must combine this with its own keep mask so it never reads
    // dst[i] on skipped rows. When nullptr (default), the decoder takes
    // the original bit-identical path — no per-row overhead.
    bool ReadColumnInto(idx_t rg_idx, idx_t col_idx, ParquetColumnData &out,
                        bool skip_str_data = false,
                        const std::vector<uint8_t> *filter_mask = nullptr);

    // Streaming: read one row group directly into a DataChunk.
    // If projection is non-empty, only loads columns where projection[col]==true.
    // Returns rows read; chunk is filled.
    idx_t ReadRowGroupChunk(idx_t rg_idx, DataChunk &chunk,
                             const std::vector<bool> &projection = {});

    // Q4 dict-histogram fast path: for an INT64 dict-encoded column,
    // accumulate (count, sum) across the row group WITHOUT materializing
    // the decoded i64_data buffer. Walks each data page, RLE-decodes
    // dict indices into a histogram count[idx], then reduces
    // sum = sum_idx (count[idx] * dict.i64[idx]).
    // Returns false if the column is not BIGINT or any data page is NOT
    // dict-encoded (PLAIN page) or any null is observed — caller falls
    // back to the standard decode path.
    bool DecodeBigintColumnHistogram(idx_t rg_idx, idx_t col_idx,
                                     int64_t &out_count, double &out_sum);

    idx_t NumRowGroups() const { return static_cast<idx_t>(meta_.row_groups.size()); }

    // Check if a row group might contain rows matching a predicate.
    bool RowGroupMightMatch(idx_t rg_idx, idx_t col_idx,
                            const std::string &op, const Value &val) const;

    // Read only the dictionary page for a VARCHAR column in a row group.
    // Used for filter pre-checks: e.g. URL LIKE '%google%' — if no dict entry
    // contains 'google', the entire RG can be pruned without decompressing
    // any data page. Mirrors DuckDB's DictionaryDecoder::InitializeDictionary
    // + HasFilteredOutAllValues path. Returns false if the column has no
    // dict page, isn't VARCHAR, or read fails. On true, out_strs has pointers
    // into out_heap (caller owns both buffers).
    bool ReadStringDictOnly(idx_t rg_idx, idx_t col_idx,
                            std::vector<const char *> &out_str_ptrs,
                            std::vector<uint32_t> &out_str_lens,
                            std::vector<char> &out_heap) const;

private:
    void ReadMetadata();
    std::vector<Value> ReadColumnChunk(const ParquetColumnMeta &meta);
    // Thrift parsing helpers.
    int32_t ReadThriftVarInt(const uint8_t *data, size_t &pos);
    int64_t ReadThriftVarInt64(const uint8_t *data, size_t &pos);
    // Returns true iff the row group can be PROVABLY pruned for op="=" against
    // an INT64 column by inspecting its dictionary page (literal absent from
    // dict). Returns false on any unsupported case (degrade safely).
    bool DictSkipPossible(idx_t rg_idx, idx_t col_idx,
                          const std::string &op, const Value &val) const;

    std::string path_;
    ParquetFileMeta meta_;
    bool meta_read_ = false;
    // True when the file is a standards-compliant Parquet (Thrift metadata),
    // false when it is our legacy custom format.
    bool is_standard_parquet_ = false;

    // Memory-mapped file: single read-only mapping shared across all decode
    // calls (including concurrent calls from worker threads). Eliminates per-
    // column `std::ifstream` open overhead for large files.
    const uint8_t *file_data_ = nullptr;
    size_t file_size_ = 0;
    bool owns_mmap_ = false;   // true if we created the mapping (must unmap in ~)
    bool owns_buffer_ = false; // true if we malloc'd a buffer (fread fallback)
    void *mmap_handle_ = nullptr; // Windows: HANDLE; POSIX: unused
};

} // namespace slothdb

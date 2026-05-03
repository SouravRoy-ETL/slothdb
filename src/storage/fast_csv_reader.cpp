#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#pragma warning(disable: 4996)
#endif

#include "slothdb/storage/fast_csv_reader.hpp"
#include "slothdb/storage/data_table.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/parallel.hpp"
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define SLOTHDB_HAS_SSE2 1
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace slothdb_internal {

// SSE2-inlined: find first byte equal to `target` starting at p (up to end).
// Returns pointer to match or end if not found. Faster than memchr for
// short-to-medium scans due to no function call overhead.
static inline const char *find_char_simd(const char *p, const char *end, char target) {
#ifdef SLOTHDB_HAS_SSE2
    const __m128i tv = _mm_set1_epi8(target);
    // Align first.
    while (p < end && (reinterpret_cast<uintptr_t>(p) & 15)) {
        if (*p == target) return p;
        p++;
    }
    while (p + 16 <= end) {
        __m128i chunk = _mm_load_si128(reinterpret_cast<const __m128i *>(p));
        __m128i cmp = _mm_cmpeq_epi8(chunk, tv);
        int mask = _mm_movemask_epi8(cmp);
        if (mask) {
#ifdef _MSC_VER
            unsigned long idx;
            _BitScanForward(&idx, static_cast<unsigned long>(mask));
            return p + idx;
#else
            return p + __builtin_ctz(mask);
#endif
        }
        p += 16;
    }
    while (p < end) { if (*p == target) return p; p++; }
    return end;
#else
    while (p < end) { if (*p == target) return p; p++; }
    return end;
#endif
}

// Find first byte equal to either c1 or c2. Used for delim-or-newline scans.
static inline const char *find_char2_simd(const char *p, const char *end, char c1, char c2) {
#ifdef SLOTHDB_HAS_SSE2
    const __m128i v1 = _mm_set1_epi8(c1);
    const __m128i v2 = _mm_set1_epi8(c2);
    while (p < end && (reinterpret_cast<uintptr_t>(p) & 15)) {
        if (*p == c1 || *p == c2) return p;
        p++;
    }
    while (p + 16 <= end) {
        __m128i chunk = _mm_load_si128(reinterpret_cast<const __m128i *>(p));
        __m128i cmp = _mm_or_si128(_mm_cmpeq_epi8(chunk, v1), _mm_cmpeq_epi8(chunk, v2));
        int mask = _mm_movemask_epi8(cmp);
        if (mask) {
#ifdef _MSC_VER
            unsigned long idx;
            _BitScanForward(&idx, static_cast<unsigned long>(mask));
            return p + idx;
#else
            return p + __builtin_ctz(mask);
#endif
        }
        p += 16;
    }
    while (p < end) { if (*p == c1 || *p == c2) return p; p++; }
    return end;
#else
    while (p < end) { if (*p == c1 || *p == c2) return p; p++; }
    return end;
#endif
}

// Find first byte equal to any of c1, c2, c3. Used to scan for the end of an
// unquoted CSV field in one pass - delim / '\n' / '\r'.
static inline const char *find_char3_simd(const char *p, const char *end,
                                          char c1, char c2, char c3) {
#ifdef SLOTHDB_HAS_SSE2
    const __m128i v1 = _mm_set1_epi8(c1);
    const __m128i v2 = _mm_set1_epi8(c2);
    const __m128i v3 = _mm_set1_epi8(c3);
    while (p < end && (reinterpret_cast<uintptr_t>(p) & 15)) {
        if (*p == c1 || *p == c2 || *p == c3) return p;
        p++;
    }
    while (p + 16 <= end) {
        __m128i chunk = _mm_load_si128(reinterpret_cast<const __m128i *>(p));
        __m128i cmp = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(chunk, v1), _mm_cmpeq_epi8(chunk, v2)),
            _mm_cmpeq_epi8(chunk, v3));
        int mask = _mm_movemask_epi8(cmp);
        if (mask) {
#ifdef _MSC_VER
            unsigned long idx;
            _BitScanForward(&idx, static_cast<unsigned long>(mask));
            return p + idx;
#else
            return p + __builtin_ctz(mask);
#endif
        }
        p += 16;
    }
    while (p < end) { if (*p == c1 || *p == c2 || *p == c3) return p; p++; }
    return end;
#else
    while (p < end) { if (*p == c1 || *p == c2 || *p == c3) return p; p++; }
    return end;
#endif
}

} // namespace slothdb_internal

#ifdef _WIN32
#include <windows.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace slothdb {

FastCSVReader::FastCSVReader(const std::string &path, char delimiter, bool has_header)
    : FastCSVReader(path, delimiter, has_header, 0) {}

FastCSVReader::FastCSVReader(const std::string &path, char delimiter, bool has_header, size_t sample_bytes)
    : delimiter_(delimiter), has_header_(has_header) {
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open CSV: " + path);
    }

    LARGE_INTEGER file_size_li;
    GetFileSizeEx(hFile, &file_size_li);
    size_t file_size = static_cast<size_t>(file_size_li.QuadPart);

    // For sample-only loading, just fread (mmap is overkill for tiny reads).
    if (sample_bytes > 0 && sample_bytes < file_size) {
        size_ = sample_bytes;
        buffer_ = static_cast<char *>(malloc(size_ + 1));
        if (!buffer_) { CloseHandle(hFile); throw InternalException("Out of memory"); }
        DWORD bytes_read;
        ReadFile(hFile, buffer_, static_cast<DWORD>(size_), &bytes_read, NULL);
        size_ = bytes_read;
        buffer_[size_] = '\0';
        CloseHandle(hFile);
        owns_buffer_ = true;
    } else if (file_size < 4 * 1024 * 1024) {
        // Small files: fread.
        size_ = file_size;
        buffer_ = static_cast<char *>(malloc(size_ + 1));
        if (!buffer_) { CloseHandle(hFile); throw InternalException("Out of memory"); }
        DWORD bytes_read;
        ReadFile(hFile, buffer_, static_cast<DWORD>(size_), &bytes_read, NULL);
        size_ = bytes_read;
        buffer_[size_] = '\0';
        CloseHandle(hFile);
        owns_buffer_ = true;
    } else {
        // Large files: memory-map.
        size_ = file_size;
        HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        CloseHandle(hFile);
        if (!hMap) throw IOException(ErrorCode::FILE_READ_ERROR, "Cannot mmap CSV: " + path);
        buffer_ = static_cast<char *>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
        mmap_handle_ = hMap;
        if (!buffer_) {
            CloseHandle(hMap);
            throw IOException(ErrorCode::FILE_READ_ERROR, "MapViewOfFile failed: " + path);
        }
        owns_buffer_ = false;
    }
#else
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open CSV: " + path);

    struct stat st;
    fstat(fd, &st);
    size_t file_size = static_cast<size_t>(st.st_size);

    if (sample_bytes > 0 && sample_bytes < file_size) {
        size_ = sample_bytes;
        buffer_ = static_cast<char *>(malloc(size_ + 1));
        if (!buffer_) { close(fd); throw InternalException("Out of memory"); }
        size_ = read(fd, buffer_, size_);
        buffer_[size_] = '\0';
        close(fd);
        owns_buffer_ = true;
    } else {
        size_ = file_size;
        buffer_ = static_cast<char *>(mmap(NULL, size_, PROT_READ, MAP_PRIVATE, fd, 0));
        close(fd);
        if (buffer_ == MAP_FAILED) {
            throw IOException(ErrorCode::FILE_READ_ERROR, "mmap failed: " + path);
        }
        owns_buffer_ = false;
    }
#endif
}

FastCSVReader::FastCSVReader(const char *buffer, size_t start, size_t end, char delimiter)
    : delimiter_(delimiter), has_header_(false), header_read_(true) {
    // Borrowed buffer - no ownership, no header parsing.
    buffer_ = const_cast<char *>(buffer);
    size_ = end;
    pos_ = start;
    owns_buffer_ = false;
    mmap_handle_ = nullptr;
}

FastCSVReader::~FastCSVReader() {
    if (owns_buffer_) {
        free(buffer_);
    } else if (mmap_handle_) {
        // Only unmap if we own the mmap handle - borrowed readers don't have one.
#ifdef _WIN32
        UnmapViewOfFile(buffer_);
        CloseHandle(mmap_handle_);
#else
        munmap(buffer_, size_);
#endif
    }
    // If !owns_buffer_ && !mmap_handle_, the buffer is borrowed - do nothing.
}

void FastCSVReader::SkipLine() {
    while (pos_ < size_ && buffer_[pos_] != '\n') pos_++;
    if (pos_ < size_) pos_++; // skip \n
}

bool FastCSVReader::NextField(const char *&field_start, size_t &field_len) {
    if (pos_ >= size_) return false;

    char c = buffer_[pos_];

    // Check for end of line.
    if (c == '\n' || c == '\r') return false;

    // Quoted field.
    if (c == '"') {
        pos_++;
        field_start = buffer_ + pos_;
        size_t start = pos_;
        while (pos_ < size_ && buffer_[pos_] != '"') pos_++;
        field_len = pos_ - start;
        if (pos_ < size_) pos_++;
        if (pos_ < size_ && buffer_[pos_] == delimiter_) pos_++;
        return true;
    }

    // Unquoted field - SIMD-scan to the next delimiter / newline. Processes
    // 16 bytes per iteration instead of one, which is the single biggest win
    // on wide or long-field CSVs.
    field_start = buffer_ + pos_;
    const char *start_p = buffer_ + pos_;
    const char *end_p   = buffer_ + size_;
    const char *stop_p  = slothdb_internal::find_char3_simd(
        start_p, end_p, delimiter_, '\n', '\r');
    field_len = static_cast<size_t>(stop_p - start_p);
    pos_ = static_cast<size_t>(stop_p - buffer_);
    if (pos_ < size_ && buffer_[pos_] == delimiter_) pos_++;
    return true;
}

std::vector<std::string> FastCSVReader::ReadHeader() {
    std::vector<std::string> header;
    if (!has_header_ || header_read_) return header;
    header_read_ = true;

    const char *field;
    size_t len;
    while (NextField(field, len)) {
        header.emplace_back(field, len);
    }
    // Skip newline.
    if (pos_ < size_ && buffer_[pos_] == '\r') pos_++;
    if (pos_ < size_ && buffer_[pos_] == '\n') pos_++;

    num_fields_ = static_cast<idx_t>(header.size());
    return header;
}

std::vector<LogicalType> FastCSVReader::DetectTypes(idx_t sample_size) {
    size_t saved_pos = pos_;

    if (has_header_ && !header_read_) ReadHeader();

    std::vector<LogicalType> types;
    idx_t rows_read = 0;

    while (rows_read < sample_size && pos_ < size_) {
        const char *field;
        size_t len;
        idx_t col = 0;

        while (NextField(field, len)) {
            if (col >= static_cast<idx_t>(types.size()))
                types.push_back(LogicalType::VARCHAR());

            if (len == 0) { col++; continue; }

            // Try TIMESTAMP first: 'YYYY-MM-DD HH:MM:SS' would otherwise match
            // BIGINT via stoll(2012, garbage_trail).
            int64_t ts_micros;
            if (Value::TryParseTimestampMicros(field, len, ts_micros)) {
                if (types[col].id() == LogicalTypeId::VARCHAR ||
                    types[col].id() == LogicalTypeId::TIMESTAMP)
                    types[col] = LogicalType::TIMESTAMP();
                col++;
                continue;
            }
            if (types[col].id() == LogicalTypeId::TIMESTAMP) {
                types[col] = LogicalType::VARCHAR();
                col++;
                continue;
            }

            // Try integer.
            bool is_int = true;
            for (size_t i = (field[0] == '-' ? 1 : 0); i < len; i++) {
                if (field[i] < '0' || field[i] > '9') { is_int = false; break; }
            }
            if (is_int && len > 0 && !(len == 1 && field[0] == '-')) {
                if (types[col].id() == LogicalTypeId::VARCHAR)
                    types[col] = LogicalType::BIGINT();
                col++;
                continue;
            }

            // Try double.
            bool is_double = true;
            bool has_dot = false;
            for (size_t i = (field[0] == '-' ? 1 : 0); i < len; i++) {
                if (field[i] == '.') { if (has_dot) { is_double = false; break; } has_dot = true; }
                else if (field[i] < '0' || field[i] > '9') { is_double = false; break; }
            }
            if (is_double && has_dot) {
                if (types[col].id() == LogicalTypeId::VARCHAR || types[col].id() == LogicalTypeId::BIGINT)
                    types[col] = LogicalType::DOUBLE();
                col++;
                continue;
            }

            types[col] = LogicalType::VARCHAR();
            col++;
        }

        // Skip newline.
        if (pos_ < size_ && buffer_[pos_] == '\r') pos_++;
        if (pos_ < size_ && buffer_[pos_] == '\n') pos_++;
        rows_read++;
    }

    // Reset.
    pos_ = saved_pos;
    header_read_ = false;
    if (num_fields_ == 0) num_fields_ = static_cast<idx_t>(types.size());

    return types;
}

int64_t FastCSVReader::ParseInt64(const char *s, size_t len) {
    if (len == 0) return 0;
    bool neg = (s[0] == '-');
    int64_t result = 0;
    for (size_t i = neg ? 1 : 0; i < len; i++) {
        result = result * 10 + (s[i] - '0');
    }
    return neg ? -result : result;
}

double FastCSVReader::ParseDouble(const char *s, size_t len) {
    // Always copy to stack - buffer may be mmap'd read-only.
    char buf[64];
    size_t copy_len = len < 63 ? len : 63;
    memcpy(buf, s, copy_len);
    buf[copy_len] = '\0';
    return strtod(buf, nullptr);
}

size_t FastCSVReader::FindLineStart(const char *buf, size_t size, size_t offset) {
    while (offset < size && buf[offset] != '\n') offset++;
    if (offset < size) offset++; // skip the newline
    return offset;
}

// Count newlines in [start, end) using SWAR.
static idx_t CountNewlinesRange(const char *start, const char *end) {
    idx_t count = 0;
    const char *p = start;

    // Align to 8-byte boundary.
    while (p < end && (reinterpret_cast<uintptr_t>(p) & 7)) {
        if (*p == '\n') count++;
        p++;
    }

    // SWAR: process 8 bytes at a time.
    const uint64_t newline_mask = 0x0A0A0A0A0A0A0A0AULL;
    while (p + 8 <= end) {
        uint64_t v;
        memcpy(&v, p, 8);
        uint64_t x = v ^ newline_mask;
        uint64_t zero_check = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
        if (zero_check) {
            for (int i = 0; i < 8; i++) if (p[i] == '\n') count++;
        }
        p += 8;
    }

    while (p < end) {
        if (*p == '\n') count++;
        p++;
    }
    return count;
}

idx_t FastCSVReader::CountRows() {
    if (has_header_ && !header_read_) ReadHeader();

    const char *start = buffer_ + pos_;
    const char *end = buffer_ + size_;
    size_t total_bytes = end - start;

    // For small ranges, single-threaded.
    if (total_bytes < 16 * 1024 * 1024) {
        idx_t count = CountNewlinesRange(start, end);
        if (size_ > 0 && buffer_[size_ - 1] != '\n') count++;
        pos_ = size_;
        return count;
    }

    // Parallel: split across threads.
    unsigned int num_threads = HWThreads();
    if (num_threads > 16) num_threads = 8;
    if (num_threads > total_bytes / (1024 * 1024)) num_threads = 1;

    std::vector<idx_t> partial(num_threads, 0);
    size_t chunk = total_bytes / num_threads;
    auto count_one = [&](unsigned int t) {
        const char *s = start + t * chunk;
        const char *e = (t == num_threads - 1) ? end : (start + (t + 1) * chunk);
        partial[t] = CountNewlinesRange(s, e);
    };
    if (num_threads > 1) {
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (unsigned int t = 0; t < num_threads; t++)
            threads.emplace_back(count_one, t);
        for (auto &th : threads) th.join();
    } else {
        count_one(0);
    }

    idx_t total = 0;
    for (auto c : partial) total += c;
    if (size_ > 0 && buffer_[size_ - 1] != '\n') total++;
    pos_ = size_;
    return total;
}

idx_t FastCSVReader::ReadChunk(DataChunk &chunk, const std::vector<LogicalType> &types) {
    if (has_header_ && !header_read_) ReadHeader();

    idx_t num_cols = static_cast<idx_t>(types.size());
    idx_t row_count = 0;

    // Get raw data pointers for each column - write directly, no Value objects.
    struct ColPtr {
        LogicalTypeId type_id;
        data_ptr_t data;
        ValidityMask *validity;
        VectorStringBuffer *str_buf;
    };
    std::vector<ColPtr> col_ptrs(num_cols);
    for (idx_t c = 0; c < num_cols; c++) {
        auto &vec = chunk.GetVector(c);
        col_ptrs[c].type_id = types[c].id();
        col_ptrs[c].data = vec.GetData();
        col_ptrs[c].validity = &vec.GetValidity();
        col_ptrs[c].str_buf = (col_ptrs[c].type_id == LogicalTypeId::VARCHAR)
                              ? &vec.GetStringBuffer() : nullptr;
    }

    while (row_count < VECTOR_SIZE && pos_ < size_) {
        const char *field;
        size_t len;
        idx_t col = 0;

        while (col < num_cols && NextField(field, len)) {
            auto &cp = col_ptrs[col];
            if (len == 0) {
                // NULL value.
                cp.validity->SetInvalid(row_count);
            } else {
                switch (cp.type_id) {
                case LogicalTypeId::BIGINT: {
                    auto *arr = reinterpret_cast<int64_t *>(cp.data);
                    arr[row_count] = ParseInt64(field, len);
                    break;
                }
                case LogicalTypeId::INTEGER: {
                    auto *arr = reinterpret_cast<int32_t *>(cp.data);
                    arr[row_count] = static_cast<int32_t>(ParseInt64(field, len));
                    break;
                }
                case LogicalTypeId::DOUBLE: {
                    auto *arr = reinterpret_cast<double *>(cp.data);
                    arr[row_count] = ParseDouble(field, len);
                    break;
                }
                case LogicalTypeId::FLOAT: {
                    auto *arr = reinterpret_cast<float *>(cp.data);
                    arr[row_count] = static_cast<float>(ParseDouble(field, len));
                    break;
                }
                case LogicalTypeId::VARCHAR: {
                    auto *arr = reinterpret_cast<string_t *>(cp.data);
                    if (len <= string_t::INLINE_LENGTH) {
                        arr[row_count] = string_t(field, static_cast<uint32_t>(len));
                    } else {
                        const char *heap = cp.str_buf->AddString(field, len);
                        arr[row_count] = string_t(heap, static_cast<uint32_t>(len));
                    }
                    break;
                }
                case LogicalTypeId::TIMESTAMP: {
                    auto *arr = reinterpret_cast<int64_t *>(cp.data);
                    int64_t micros;
                    if (Value::TryParseTimestampMicros(field, len, micros)) {
                        arr[row_count] = micros;
                    } else {
                        cp.validity->SetInvalid(row_count);
                    }
                    break;
                }
                default: {
                    // Fallback for other types - use SetValue.
                    chunk.SetValue(col, row_count, Value::VARCHAR(std::string(field, len)));
                    break;
                }
                }
            }
            col++;
        }
        // Fill remaining columns with NULL.
        while (col < num_cols) {
            col_ptrs[col].validity->SetInvalid(row_count);
            col++;
        }

        // Skip remaining fields on this line.
        while (pos_ < size_ && buffer_[pos_] != '\n' && buffer_[pos_] != '\r') {
            pos_++;
        }
        if (pos_ < size_ && buffer_[pos_] == '\r') pos_++;
        if (pos_ < size_ && buffer_[pos_] == '\n') pos_++;

        row_count++;
    }

    chunk.SetCardinality(row_count);
    return row_count;
}

idx_t FastCSVReader::ReadChunkProjected(DataChunk &chunk, const std::vector<LogicalType> &types,
                                         const std::vector<bool> &projection) {
    if (has_header_ && !header_read_) ReadHeader();

    idx_t num_cols = static_cast<idx_t>(types.size());
    idx_t row_count = 0;

    // Find last needed column - skip to newline after it (big perf win).
    idx_t last_needed = 0;
    for (idx_t c = 0; c < num_cols; c++) {
        if (c < projection.size() && projection[c]) last_needed = c;
    }

    // Pre-resolve raw data pointers per column.
    struct ColPtr {
        LogicalTypeId type_id;
        data_ptr_t data;
        ValidityMask *validity;
        VectorStringBuffer *str_buf;
        bool needed;
    };
    std::vector<ColPtr> col_ptrs(num_cols);
    for (idx_t c = 0; c < num_cols; c++) {
        auto &vec = chunk.GetVector(c);
        col_ptrs[c].type_id = types[c].id();
        col_ptrs[c].data = vec.GetData();
        col_ptrs[c].validity = &vec.GetValidity();
        col_ptrs[c].str_buf = (col_ptrs[c].type_id == LogicalTypeId::VARCHAR)
                              ? &vec.GetStringBuffer() : nullptr;
        col_ptrs[c].needed = (c < projection.size()) ? projection[c] : false;
    }

    while (row_count < VECTOR_SIZE && pos_ < size_) {
        const char *field;
        size_t len;
        idx_t col = 0;

        // Skip past all unneeded columns before the first needed one using inline SIMD.
        idx_t skip_count = 0;
        while (col <= last_needed && !col_ptrs[col].needed) { skip_count++; col++; }
        if (skip_count > 0) {
            for (idx_t s = 0; s < skip_count; s++) {
                if (pos_ >= size_) break;
                const char *d = slothdb_internal::find_char_simd(
                    buffer_ + pos_, buffer_ + size_, delimiter_);
                if (d == buffer_ + size_) { pos_ = size_; break; }
                pos_ = (size_t)(d - buffer_ + 1);
            }
        }
        while (col <= last_needed) {
            auto &cp = col_ptrs[col];
            if (!cp.needed) {
                // Byte-by-byte fallback for middle-of-row skipped cols.
                if (pos_ >= size_) break;
                if (buffer_[pos_] == '\n' || buffer_[pos_] == '\r') break;
                if (buffer_[pos_] == '"') {
                    pos_++;
                    while (pos_ < size_ && buffer_[pos_] != '"') pos_++;
                    if (pos_ < size_) pos_++;
                    if (pos_ < size_ && buffer_[pos_] == delimiter_) pos_++;
                } else {
                    while (pos_ < size_ && buffer_[pos_] != delimiter_ &&
                           buffer_[pos_] != '\n' && buffer_[pos_] != '\r') pos_++;
                    if (pos_ < size_ && buffer_[pos_] == delimiter_) pos_++;
                }
                col++;
                continue;
            }
            if (!NextField(field, len)) break;
            if (len == 0) {
                cp.validity->SetInvalid(row_count);
            } else {
                switch (cp.type_id) {
                case LogicalTypeId::BIGINT: {
                    auto *arr = reinterpret_cast<int64_t *>(cp.data);
                    arr[row_count] = ParseInt64(field, len);
                    break;
                }
                case LogicalTypeId::INTEGER: {
                    auto *arr = reinterpret_cast<int32_t *>(cp.data);
                    arr[row_count] = static_cast<int32_t>(ParseInt64(field, len));
                    break;
                }
                case LogicalTypeId::DOUBLE: {
                    auto *arr = reinterpret_cast<double *>(cp.data);
                    arr[row_count] = ParseDouble(field, len);
                    break;
                }
                case LogicalTypeId::FLOAT: {
                    auto *arr = reinterpret_cast<float *>(cp.data);
                    arr[row_count] = static_cast<float>(ParseDouble(field, len));
                    break;
                }
                case LogicalTypeId::VARCHAR: {
                    auto *arr = reinterpret_cast<string_t *>(cp.data);
                    if (len <= string_t::INLINE_LENGTH) {
                        arr[row_count] = string_t(field, static_cast<uint32_t>(len));
                    } else {
                        const char *heap = cp.str_buf->AddString(field, len);
                        arr[row_count] = string_t(heap, static_cast<uint32_t>(len));
                    }
                    break;
                }
                case LogicalTypeId::TIMESTAMP: {
                    auto *arr = reinterpret_cast<int64_t *>(cp.data);
                    int64_t micros;
                    if (Value::TryParseTimestampMicros(field, len, micros)) {
                        arr[row_count] = micros;
                    } else {
                        cp.validity->SetInvalid(row_count);
                    }
                    break;
                }
                default:
                    chunk.SetValue(col, row_count, Value::VARCHAR(std::string(field, len)));
                    break;
                }
            }
            col++;
        }
        while (col < num_cols) {
            if (col_ptrs[col].needed) col_ptrs[col].validity->SetInvalid(row_count);
            col++;
        }

        // Fast skip to newline using inline SIMD.
        if (pos_ < size_) {
            const char *nl = slothdb_internal::find_char_simd(
                buffer_ + pos_, buffer_ + size_, '\n');
            pos_ = (nl == buffer_ + size_) ? size_ : (size_t)(nl - buffer_ + 1);
        }

        row_count++;
    }

    chunk.SetCardinality(row_count);
    return row_count;
}

void FastCSVReader::ReadIntoTable(DataTable &table, const std::vector<LogicalType> &types) {
    DataChunk chunk;
    chunk.Initialize(types);

    while (!IsEOF()) {
        chunk.Reset();
        idx_t count = ReadChunk(chunk, types);
        if (count == 0) break;
        table.Append(chunk);
    }
}

void FastCSVReader::ReadIntoChunks(std::vector<DataChunk> &out,
                                    const std::vector<LogicalType> &types,
                                    const std::vector<bool> *projection) {
    if (has_header_ && !header_read_) ReadHeader();

    size_t start = pos_;
    size_t end = size_;
    size_t range = end - start;

    // Small inputs (< ~2 MB) - single-threaded; parallel overhead dominates.
    unsigned int nt = (range < 2 * 1024 * 1024) ? 1u : HWThreads();
    if (nt > 8) nt = 8;
    if (range < 256 * 1024 * nt) nt = 1;

    auto parse_range = [&](size_t s, size_t e, std::vector<DataChunk> &local) {
        FastCSVReader worker(buffer_, s, e, delimiter_);
        while (!worker.IsEOF()) {
            DataChunk chunk;
            chunk.Initialize(types);
            idx_t count = projection
                ? worker.ReadChunkProjected(chunk, types, *projection)
                : worker.ReadChunk(chunk, types);
            if (count == 0) break;
            local.push_back(std::move(chunk));
        }
    };

    if (nt <= 1) {
        parse_range(start, end, out);
        pos_ = end;
        return;
    }

    std::vector<std::pair<size_t, size_t>> ranges;
    ranges.reserve(nt);
    size_t chunk_bytes = range / nt;
    size_t cur = start;
    for (unsigned int t = 0; t < nt; t++) {
        size_t next;
        if (t == nt - 1) {
            next = end;
        } else {
            size_t tentative = start + (t + 1) * chunk_bytes;
            // Snap to next line start so we don't split rows.
            next = FindLineStart(buffer_, end, tentative);
        }
        if (cur < next) ranges.emplace_back(cur, next);
        cur = next;
    }

    std::vector<std::vector<DataChunk>> per_thread(ranges.size());
    std::vector<std::thread> threads;
    threads.reserve(ranges.size());
    for (size_t i = 0; i < ranges.size(); i++) {
        threads.emplace_back([&, i] {
            parse_range(ranges[i].first, ranges[i].second, per_thread[i]);
        });
    }
    for (auto &th : threads) th.join();

    for (auto &v : per_thread) {
        for (auto &c : v) out.push_back(std::move(c));
    }
    pos_ = end;
}

} // namespace slothdb

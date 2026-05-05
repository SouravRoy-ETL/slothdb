#include "slothdb/storage/parquet.hpp"
#include "slothdb/storage/parquet_lazy_strdata.hpp"
#include "slothdb/common/exception.hpp"
#include "miniz.h"
#include "zstd.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace slothdb {

static const char PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};

// =============================================================================
// Standard Parquet support (Thrift metadata + Snappy + PLAIN / PLAIN_DICTIONARY
// + RLE/bit-packed hybrid). Coexists with the legacy custom-format writer below.
// =============================================================================
namespace {

// --- Snappy (raw, no CRC) decompressor. Returns false on corruption. ---
bool SnappyDecompress(const uint8_t *in, size_t in_size, std::vector<uint8_t> &out) {
    if (in_size == 0) { out.clear(); return true; }
    size_t p = 0;
    uint64_t uncomp_len = 0;
    int shift = 0;
    while (p < in_size) {
        uint8_t b = in[p++];
        uncomp_len |= uint64_t(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
        if (shift > 35) return false;
    }
    out.resize(static_cast<size_t>(uncomp_len));
    size_t op = 0;
    while (p < in_size) {
        uint8_t tag = in[p++];
        uint8_t tt = tag & 0x03;
        if (tt == 0) {
            uint32_t len;
            uint32_t hi = tag >> 2;
            if (hi < 60) len = hi + 1;
            else if (hi == 60) { if (p + 1 > in_size) return false; len = 1u + in[p]; p += 1; }
            else if (hi == 61) { if (p + 2 > in_size) return false; len = 1u + uint32_t(in[p]) + (uint32_t(in[p+1]) << 8); p += 2; }
            else if (hi == 62) { if (p + 3 > in_size) return false; len = 1u + uint32_t(in[p]) + (uint32_t(in[p+1]) << 8) + (uint32_t(in[p+2]) << 16); p += 3; }
            else { if (p + 4 > in_size) return false; len = 1u + uint32_t(in[p]) + (uint32_t(in[p+1]) << 8) + (uint32_t(in[p+2]) << 16) + (uint32_t(in[p+3]) << 24); p += 4; }
            if (p + len > in_size || op + len > out.size()) return false;
            std::memcpy(&out[op], &in[p], len);
            p += len; op += len;
        } else if (tt == 1) {
            if (p + 1 > in_size) return false;
            uint32_t len = ((tag >> 2) & 0x07) + 4;
            uint32_t off = (uint32_t(tag >> 5) << 8) | in[p++];
            if (off == 0 || off > op || op + len > out.size()) return false;
            for (uint32_t i = 0; i < len; i++) out[op + i] = out[op - off + i];
            op += len;
        } else if (tt == 2) {
            if (p + 2 > in_size) return false;
            uint32_t len = (tag >> 2) + 1;
            uint32_t off = uint32_t(in[p]) | (uint32_t(in[p+1]) << 8);
            p += 2;
            if (off == 0 || off > op || op + len > out.size()) return false;
            for (uint32_t i = 0; i < len; i++) out[op + i] = out[op - off + i];
            op += len;
        } else {
            if (p + 4 > in_size) return false;
            uint32_t len = (tag >> 2) + 1;
            uint32_t off = uint32_t(in[p]) | (uint32_t(in[p+1]) << 8) | (uint32_t(in[p+2]) << 16) | (uint32_t(in[p+3]) << 24);
            p += 4;
            if (off == 0 || off > op || op + len > out.size()) return false;
            for (uint32_t i = 0; i < len; i++) out[op + i] = out[op - off + i];
            op += len;
        }
    }
    return op == static_cast<size_t>(uncomp_len);
}

// --- Thrift Compact Protocol reader. ---
struct ThriftRd {
    const uint8_t *data;
    size_t size;
    size_t pos = 0;
    // Field-id delta stack (one entry per open struct).
    std::vector<int16_t> stack;
    int16_t last_fid = 0;

    ThriftRd(const uint8_t *d, size_t s) : data(d), size(s) {}

    bool End() const { return pos >= size; }
    uint8_t Byte() { return data[pos++]; }
    uint64_t VarU64() {
        uint64_t r = 0; int sh = 0;
        while (pos < size) {
            uint8_t b = data[pos++];
            r |= uint64_t(b & 0x7F) << sh;
            if ((b & 0x80) == 0) return r;
            sh += 7;
            if (sh > 63) return r;
        }
        return r;
    }
    int32_t ZigI32() { uint64_t v = VarU64(); return static_cast<int32_t>((v >> 1) ^ (~(v & 1) + 1)); }
    int64_t ZigI64() { uint64_t v = VarU64(); return static_cast<int64_t>((v >> 1) ^ (~(v & 1) + 1)); }
    double Dbl() { double v; std::memcpy(&v, &data[pos], 8); pos += 8; return v; }
    std::string Str() {
        uint64_t l = VarU64();
        if (pos + l > size) { pos = size; return ""; }
        std::string s(reinterpret_cast<const char*>(&data[pos]), l);
        pos += l;
        return s;
    }
    void StructBegin() { stack.push_back(last_fid); last_fid = 0; }
    void StructEnd() { if (!stack.empty()) { last_fid = stack.back(); stack.pop_back(); } }
    // Returns true if field present; sets fid, tid. tid==0 marks end of struct.
    bool Field(int16_t &fid, uint8_t &tid) {
        if (End()) { tid = 0; return false; }
        uint8_t h = Byte();
        if (h == 0) { tid = 0; return false; }
        tid = h & 0x0F;
        int delta = h >> 4;
        if (delta == 0) {
            uint64_t raw = VarU64();
            fid = static_cast<int16_t>((raw >> 1) ^ (~(raw & 1) + 1));
        } else {
            fid = last_fid + delta;
        }
        last_fid = fid;
        return true;
    }
    // List/set header: (count << 4) | elem_type, count==15 => varint count.
    void ListBeg(uint8_t &elem_type, uint32_t &count) {
        uint8_t h = Byte();
        count = h >> 4;
        elem_type = h & 0x0F;
        if (count == 15) count = static_cast<uint32_t>(VarU64());
    }
    void Skip(uint8_t tid) {
        switch (tid) {
        case 1: case 2: break;
        case 3: pos += 1; break;
        case 4: ZigI32(); break;
        case 5: ZigI32(); break;
        case 6: ZigI64(); break;
        case 7: pos += 8; break;
        case 8: { uint64_t l = VarU64(); pos += l; break; }
        case 9: case 10: {
            uint8_t et; uint32_t cnt; ListBeg(et, cnt);
            for (uint32_t i = 0; i < cnt; i++) Skip(et);
            break;
        }
        case 11: {
            uint32_t cnt = static_cast<uint32_t>(VarU64());
            if (cnt > 0) {
                uint8_t types = Byte();
                uint8_t kt = types >> 4;
                uint8_t vt = types & 0x0F;
                for (uint32_t i = 0; i < cnt; i++) { Skip(kt); Skip(vt); }
            }
            break;
        }
        case 12: {
            StructBegin();
            int16_t fid; uint8_t t;
            while (Field(fid, t)) Skip(t);
            StructEnd();
            break;
        }
        default: break;
        }
    }
};

// --- Parquet metadata structs (subset we actually read). ---
struct ParqSchemaEl {
    int32_t type = -1;           // Physical type, or -1 for a group.
    int32_t repetition_type = 0; // 0=REQUIRED, 1=OPTIONAL, 2=REPEATED.
    std::string name;
    int32_t num_children = 0;
    int32_t converted_type = -1;
    int32_t type_length = 0;
};

struct ParqColMetaFull {
    int32_t parquet_type = 0;
    std::vector<int32_t> encodings;
    int32_t codec = 0; // 0=UNCOMPRESSED, 1=SNAPPY, 2=GZIP, ...
    int64_t num_values = 0;
    int64_t total_uncompressed_size = 0;
    int64_t total_compressed_size = 0;
    int64_t data_page_offset = -1;
    int64_t dict_page_offset = -1;
    std::string name;
    // Stats (raw bytes; interpretation depends on type).
    bool has_stats = false;
    std::string min_bytes, max_bytes;
};

struct ParqRGFull {
    int64_t num_rows = 0;
    std::vector<ParqColMetaFull> columns;
};

struct ParqFileMeta {
    int32_t version = 0;
    int64_t num_rows = 0;
    std::vector<ParqSchemaEl> schema;
    std::vector<ParqRGFull> row_groups;
    bool ok = false;
};

// Forward decl.
void ParseColumnMeta(ThriftRd &r, ParqColMetaFull &m);

void ParseStatistics(ThriftRd &r, ParqColMetaFull &m) {
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 1: { // max
            uint64_t l = r.VarU64();
            m.max_bytes.assign(reinterpret_cast<const char*>(&r.data[r.pos]), l);
            r.pos += l; m.has_stats = true;
            break;
        }
        case 2: { // min
            uint64_t l = r.VarU64();
            m.min_bytes.assign(reinterpret_cast<const char*>(&r.data[r.pos]), l);
            r.pos += l; m.has_stats = true;
            break;
        }
        case 5: { // max_value (preferred over deprecated `max`)
            uint64_t l = r.VarU64();
            m.max_bytes.assign(reinterpret_cast<const char*>(&r.data[r.pos]), l);
            r.pos += l; m.has_stats = true;
            break;
        }
        case 6: { // min_value (preferred over deprecated `min`)
            uint64_t l = r.VarU64();
            m.min_bytes.assign(reinterpret_cast<const char*>(&r.data[r.pos]), l);
            r.pos += l; m.has_stats = true;
            break;
        }
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
}

void ParseSchemaElement(ThriftRd &r, ParqSchemaEl &e) {
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 1: e.type = r.ZigI32(); break;
        case 2: e.type_length = r.ZigI32(); break;
        case 3: e.repetition_type = r.ZigI32(); break;
        case 4: e.name = r.Str(); break;
        case 5: e.num_children = r.ZigI32(); break;
        case 6: e.converted_type = r.ZigI32(); break;
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
}

void ParseColumnChunkStruct(ThriftRd &r, ParqColMetaFull &m) {
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 2: { // file_offset (we prefer meta_data offsets)
            r.ZigI64();
            break;
        }
        case 3: { // meta_data (struct)
            ParseColumnMeta(r, m);
            break;
        }
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
}

void ParseColumnMeta(ThriftRd &r, ParqColMetaFull &m) {
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 1: m.parquet_type = r.ZigI32(); break;
        case 2: { // encodings list<i32>
            uint8_t et; uint32_t cnt; r.ListBeg(et, cnt);
            for (uint32_t i = 0; i < cnt; i++) m.encodings.push_back(r.ZigI32());
            break;
        }
        case 3: { // path_in_schema list<string>
            uint8_t et; uint32_t cnt; r.ListBeg(et, cnt);
            for (uint32_t i = 0; i < cnt; i++) {
                auto s = r.Str();
                if (i == cnt - 1) m.name = s;
            }
            break;
        }
        case 4: m.codec = r.ZigI32(); break;
        case 5: m.num_values = r.ZigI64(); break;
        case 6: m.total_uncompressed_size = r.ZigI64(); break;
        case 7: m.total_compressed_size = r.ZigI64(); break;
        case 9: m.data_page_offset = r.ZigI64(); break;
        case 11: m.dict_page_offset = r.ZigI64(); break;
        case 12: ParseStatistics(r, m); break;
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
}

void ParseRowGroup(ThriftRd &r, ParqRGFull &rg) {
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 1: { // columns list<ColumnChunk>
            uint8_t et; uint32_t cnt; r.ListBeg(et, cnt);
            rg.columns.resize(cnt);
            for (uint32_t i = 0; i < cnt; i++) ParseColumnChunkStruct(r, rg.columns[i]);
            break;
        }
        case 3: rg.num_rows = r.ZigI64(); break;
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
}

// Returns false if the footer isn't a valid Thrift FileMetaData.
bool ParseFileMeta(const uint8_t *buf, size_t len, ParqFileMeta &m) {
    ThriftRd r(buf, len);
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        if (r.pos > r.size) return false;
        switch (fid) {
        case 1: m.version = r.ZigI32(); break;
        case 2: { // schema list<SchemaElement>
            uint8_t et; uint32_t cnt; r.ListBeg(et, cnt);
            m.schema.resize(cnt);
            for (uint32_t i = 0; i < cnt; i++) ParseSchemaElement(r, m.schema[i]);
            break;
        }
        case 3: m.num_rows = r.ZigI64(); break;
        case 4: { // row_groups list<RowGroup>
            uint8_t et; uint32_t cnt; r.ListBeg(et, cnt);
            m.row_groups.resize(cnt);
            for (uint32_t i = 0; i < cnt; i++) ParseRowGroup(r, m.row_groups[i]);
            break;
        }
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
    m.ok = true;
    return true;
}

// --- Parquet page header (parsed from Thrift) ---
struct ParqPageHeader {
    int32_t type = 0; // 0=DATA_PAGE, 1=INDEX_PAGE, 2=DICTIONARY_PAGE, 3=DATA_PAGE_V2
    int32_t uncompressed_page_size = 0;
    int32_t compressed_page_size = 0;
    // For DATA_PAGE / DATA_PAGE_V2:
    int32_t num_values = 0;
    int32_t encoding = 0;
    int32_t def_level_encoding = 3; // default RLE
    int32_t rep_level_encoding = 3;
    // For DATA_PAGE_V2 only:
    int32_t def_levels_byte_length = 0;
    int32_t rep_levels_byte_length = 0;
    bool is_compressed = true;
    // For DICTIONARY_PAGE:
    int32_t dict_num_values = 0;
    int32_t dict_encoding = 0;
};

void ParseDataPageHeader(ThriftRd &r, ParqPageHeader &h) {
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 1: h.num_values = r.ZigI32(); break;
        case 2: h.encoding = r.ZigI32(); break;
        case 3: h.def_level_encoding = r.ZigI32(); break;
        case 4: h.rep_level_encoding = r.ZigI32(); break;
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
}

void ParseDictPageHeader(ThriftRd &r, ParqPageHeader &h) {
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 1: h.dict_num_values = r.ZigI32(); break;
        case 2: h.dict_encoding = r.ZigI32(); break;
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
}

void ParseDataPageV2Header(ThriftRd &r, ParqPageHeader &h) {
    h.is_compressed = true; // default is_compressed = true
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 1: h.num_values = r.ZigI32(); break;
        case 3: /* num_rows */ r.ZigI32(); break;
        case 4: h.encoding = r.ZigI32(); break;
        case 5: h.def_levels_byte_length = r.ZigI32(); break;
        case 6: h.rep_levels_byte_length = r.ZigI32(); break;
        case 7: // is_compressed (BOOL)
            if (tid == 1) h.is_compressed = true;
            else if (tid == 2) h.is_compressed = false;
            break;
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
}

// Parse a PageHeader. Returns bytes consumed.
size_t ParsePageHeader(const uint8_t *buf, size_t len, ParqPageHeader &h) {
    ThriftRd r(buf, len);
    r.StructBegin();
    int16_t fid; uint8_t tid;
    while (r.Field(fid, tid)) {
        switch (fid) {
        case 1: h.type = r.ZigI32(); break;
        case 2: h.uncompressed_page_size = r.ZigI32(); break;
        case 3: h.compressed_page_size = r.ZigI32(); break;
        case 5: ParseDataPageHeader(r, h); break;
        case 7: ParseDictPageHeader(r, h); break;
        case 8: ParseDataPageV2Header(r, h); break;
        default: r.Skip(tid); break;
        }
    }
    r.StructEnd();
    return r.pos;
}

// --- RLE/bit-packed hybrid decoder (Parquet encoding). ---
// Format: runs of either RLE (value repeated N times) or bit-packed groups.
// Header varint: if LSB=1, (header>>1) * 8 = number of packed values in this run.
//                if LSB=0, (header>>1) = repeat count; then read (bit_width+7)/8 bytes = value.
struct RleDecoder {
    const uint8_t *p;
    size_t size;
    size_t pos;
    int bit_width;
    // Current run state
    int mode = -1; // 0=rle, 1=bitpacked, -1=need new run
    uint32_t rle_value = 0;
    uint32_t rle_count = 0;
    uint32_t bp_count = 0;
    // Bit-packing buffer
    uint64_t bitbuf = 0;
    int bits_in_buf = 0;

    RleDecoder(const uint8_t *d, size_t s, int bw) : p(d), size(s), pos(0), bit_width(bw) {}

    bool FetchRun() {
        if (pos >= size) return false;
        uint64_t hdr = 0; int sh = 0;
        while (pos < size) {
            uint8_t b = p[pos++];
            hdr |= uint64_t(b & 0x7F) << sh;
            if ((b & 0x80) == 0) break;
            sh += 7;
        }
        if (hdr & 1) {
            mode = 1;
            bp_count = static_cast<uint32_t>((hdr >> 1) * 8);
            bitbuf = 0; bits_in_buf = 0;
        } else {
            mode = 0;
            rle_count = static_cast<uint32_t>(hdr >> 1);
            int bytes = (bit_width + 7) / 8;
            rle_value = 0;
            for (int i = 0; i < bytes; i++) {
                if (pos >= size) return false;
                rle_value |= uint32_t(p[pos++]) << (8 * i);
            }
        }
        return true;
    }

    bool Next(uint32_t &out) {
        while (true) {
            if (mode == 0) {
                if (rle_count > 0) { out = rle_value; rle_count--; return true; }
                mode = -1;
            } else if (mode == 1) {
                if (bp_count > 0) {
                    while (bits_in_buf < bit_width) {
                        if (pos >= size) return false;
                        bitbuf |= uint64_t(p[pos++]) << bits_in_buf;
                        bits_in_buf += 8;
                    }
                    uint32_t mask = (bit_width >= 32) ? 0xFFFFFFFFu : ((1u << bit_width) - 1u);
                    out = static_cast<uint32_t>(bitbuf & mask);
                    bitbuf >>= bit_width;
                    bits_in_buf -= bit_width;
                    bp_count--;
                    return true;
                }
                mode = -1;
            } else {
                if (!FetchRun()) return false;
            }
        }
    }

    // Bulk-extract up to n indices from whichever run is currently active. If the
    // active run is bit-packed, peel values in a tight loop without re-checking
    // mode each iteration. If RLE, splat the constant. Returns the number written
    // (may be less than n when the run is exhausted; caller drains the next run).
    int NextBatch(uint32_t *out, int n) {
        if (mode == -1) {
            if (!FetchRun()) return 0;
        }
        if (mode == 0) {
            int take = (rle_count < (uint32_t)n) ? (int)rle_count : n;
            for (int i = 0; i < take; i++) out[i] = rle_value;
            rle_count -= (uint32_t)take;
            if (rle_count == 0) mode = -1;
            return take;
        }
        // mode == 1: bit-packed
        int take = (bp_count < (uint32_t)n) ? (int)bp_count : n;
        uint32_t mask = (bit_width >= 32) ? 0xFFFFFFFFu : ((1u << bit_width) - 1u);
        int bw = bit_width;
        uint64_t buf = bitbuf;
        int bib = bits_in_buf;
        size_t pp = pos;
        const uint8_t *pd = p;
        size_t psz = size;
        for (int i = 0; i < take; i++) {
            // Refill: pull bytes until we have at least bw bits queued in `buf`.
            while (bib < bw) {
                if (pp >= psz) {
                    bitbuf = buf; bits_in_buf = bib; pos = pp; bp_count -= (uint32_t)i;
                    if (bp_count == 0) mode = -1;
                    return i;
                }
                buf |= uint64_t(pd[pp++]) << bib;
                bib += 8;
            }
            out[i] = static_cast<uint32_t>(buf & mask);
            buf >>= bw;
            bib -= bw;
        }
        bitbuf = buf; bits_in_buf = bib; pos = pp;
        bp_count -= (uint32_t)take;
        if (bp_count == 0) mode = -1;
        return take;
    }
};

// Skip the gzip header (RFC 1952) and return a pointer to the start of
// the raw deflate stream, or nullptr on malformed header. miniz doesn't
// support the gzip wrapper directly (only raw deflate or zlib), so we
// peel the wrapper here and feed the deflate body to mz_inflate with
// windowBits = -15 (raw mode).
const uint8_t *SkipGzipHeader(const uint8_t *in, size_t in_size, size_t &consumed) {
    if (in_size < 10) return nullptr;
    if (in[0] != 0x1F || in[1] != 0x8B || in[2] != 0x08) return nullptr; // bad magic / non-deflate
    uint8_t flg = in[3];
    size_t p = 10;
    if (flg & 0x04) { // FEXTRA
        if (p + 2 > in_size) return nullptr;
        uint16_t xlen = uint16_t(in[p]) | (uint16_t(in[p + 1]) << 8);
        p += 2 + xlen;
        if (p > in_size) return nullptr;
    }
    if (flg & 0x08) { // FNAME (zero-terminated)
        while (p < in_size && in[p] != 0) p++;
        if (p >= in_size) return nullptr;
        p++;
    }
    if (flg & 0x10) { // FCOMMENT (zero-terminated)
        while (p < in_size && in[p] != 0) p++;
        if (p >= in_size) return nullptr;
        p++;
    }
    if (flg & 0x02) { // FHCRC
        p += 2;
        if (p > in_size) return nullptr;
    }
    consumed = p;
    return in + p;
}

// Decompress a Parquet GZIP page. The Parquet GZIP codec wraps a raw
// deflate stream in the standard RFC 1952 gzip envelope; we strip the
// header (and ignore the 8-byte trailer, which miniz simply doesn't
// read past the deflate end-of-stream marker) and call mz_inflate in
// raw mode.
bool GzipDecompress(const uint8_t *in, size_t in_size, size_t uncompressed_size,
                    std::vector<uint8_t> &out) {
    out.resize(uncompressed_size);
    if (uncompressed_size == 0) return true;
    size_t hdr = 0;
    const uint8_t *body = SkipGzipHeader(in, in_size, hdr);
    if (!body) return false;
    size_t body_size = in_size - hdr;
    // 8-byte trailer (CRC32 + ISIZE) is past the deflate end-of-stream
    // marker; mz_inflate stops at the marker so we don't need to trim.
    mz_stream strm{};
    if (mz_inflateInit2(&strm, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return false;
    strm.next_in = const_cast<uint8_t *>(body);
    strm.avail_in = static_cast<mz_uint32>(body_size);
    strm.next_out = out.data();
    strm.avail_out = static_cast<mz_uint32>(uncompressed_size);
    int rc = mz_inflate(&strm, MZ_FINISH);
    mz_inflateEnd(&strm);
    return rc == MZ_STREAM_END && strm.total_out == uncompressed_size;
}

// Map a Parquet CompressionCodec ID to a human-readable name for error
// messages. Mirrors the Parquet Thrift enum.
const char *CodecName(int32_t codec) {
    switch (codec) {
    case 0: return "UNCOMPRESSED";
    case 1: return "SNAPPY";
    case 2: return "GZIP";
    case 3: return "LZO";
    case 4: return "BROTLI";
    case 5: return "LZ4";
    case 6: return "ZSTD";
    case 7: return "LZ4_RAW";
    default: return "UNKNOWN";
    }
}

// Decompress a Parquet ZSTD page using the vendored libzstd. Sized from
// the page header's uncompressed_size; ZSTD_decompressDCtx reuses a
// thread-local context so each call skips the implicit context allocation
// that ZSTD_decompress performs internally. Saves ~1-2us per page; on a
// 100M-row column with hundreds of pages per RG this compounds to a few
// percent on ZSTD-bound queries (Q3, Q4, Q6).
bool ZstdDecompress(const uint8_t *in, size_t in_size, size_t uncompressed_size,
                    std::vector<uint8_t> &out) {
    out.resize(uncompressed_size);
    if (uncompressed_size == 0) return true;
    struct DCtxHolder {
        ZSTD_DCtx *ctx = nullptr;
        DCtxHolder() : ctx(ZSTD_createDCtx()) {}
        ~DCtxHolder() { if (ctx) ZSTD_freeDCtx(ctx); }
    };
    static thread_local DCtxHolder holder;
    if (!holder.ctx) {
        // First-call recovery: thread_local ctor failed to allocate the
        // context (very rare). Fall back to the simple API.
        size_t got = ZSTD_decompress(out.data(), uncompressed_size, in, in_size);
        if (ZSTD_isError(got)) return false;
        return got == uncompressed_size;
    }
    size_t got = ZSTD_decompressDCtx(holder.ctx, out.data(), uncompressed_size,
                                      in, in_size);
    if (ZSTD_isError(got)) return false;
    return got == uncompressed_size;
}

// Decompress a page body. Parquet codecs we handle: UNCOMPRESSED (0),
// SNAPPY (1), GZIP (2), ZSTD (6). LZO (3) / BROTLI (4) / LZ4 (5) /
// LZ4_RAW (7) are not yet supported.
bool DecompressPage(int32_t codec, const uint8_t *in, size_t compressed_size, size_t uncompressed_size,
                    std::vector<uint8_t> &out) {
    if (codec == 0) {
        out.assign(in, in + compressed_size);
        return out.size() == uncompressed_size;
    }
    if (codec == 1) {
        return SnappyDecompress(in, compressed_size, out);
    }
    if (codec == 2) {
        return GzipDecompress(in, compressed_size, uncompressed_size, out);
    }
    if (codec == 6) {
        return ZstdDecompress(in, compressed_size, uncompressed_size, out);
    }
    return false; // unsupported
}

// Read a varint from a byte range (used by the RLE/bit-packed header length prefix).
uint32_t ReadLEU32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

} // namespace


static ParquetType LogicalToParquetType(const LogicalType &type) {
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN: return ParquetType::BOOLEAN;
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE: return ParquetType::INT32;
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP: return ParquetType::INT64;
    case LogicalTypeId::FLOAT: return ParquetType::FLOAT;
    case LogicalTypeId::DOUBLE: return ParquetType::DOUBLE;
    default: return ParquetType::BYTE_ARRAY;
    }
}

static LogicalType ParquetToLogicalType(ParquetType type) {
    switch (type) {
    case ParquetType::BOOLEAN: return LogicalType::BOOLEAN();
    case ParquetType::INT32: return LogicalType::INTEGER();
    case ParquetType::INT64: return LogicalType::BIGINT();
    case ParquetType::FLOAT: return LogicalType::FLOAT();
    case ParquetType::DOUBLE: return LogicalType::DOUBLE();
    case ParquetType::BYTE_ARRAY: return LogicalType::VARCHAR();
    }
    return LogicalType::VARCHAR();
}

// ============================================================================
// Thrift Compact Protocol Helpers
// ============================================================================

void ParquetWriter::WriteVarInt(std::vector<uint8_t> &buf, uint64_t val) {
    while (val >= 0x80) {
        buf.push_back(static_cast<uint8_t>(val | 0x80));
        val >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(val));
}

void ParquetWriter::WriteThriftFieldI32(std::vector<uint8_t> &buf, int field_id,
                                         int32_t val) {
    // Compact: field header + zigzag-encoded i32.
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 5)); // type 5 = i32
    uint32_t zigzag = static_cast<uint32_t>((val << 1) ^ (val >> 31));
    WriteVarInt(buf, zigzag);
}

void ParquetWriter::WriteThriftFieldI64(std::vector<uint8_t> &buf, int field_id,
                                         int64_t val) {
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 6)); // type 6 = i64
    uint64_t zigzag = static_cast<uint64_t>((val << 1) ^ (val >> 63));
    WriteVarInt(buf, zigzag);
}

void ParquetWriter::WriteThriftFieldString(std::vector<uint8_t> &buf, int field_id,
                                            const std::string &val) {
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 8)); // type 8 = binary
    WriteVarInt(buf, val.size());
    buf.insert(buf.end(), val.begin(), val.end());
}

void ParquetWriter::WriteThriftFieldList(std::vector<uint8_t> &buf, int field_id,
                                          int elem_type, int count) {
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 12)); // type 12 = list
    if (count < 15) {
        buf.push_back(static_cast<uint8_t>((count << 4) | elem_type));
    } else {
        buf.push_back(static_cast<uint8_t>(0xF0 | elem_type));
        WriteVarInt(buf, count);
    }
}

void ParquetWriter::WriteThriftFieldStruct(std::vector<uint8_t> &buf, int field_id) {
    buf.push_back(static_cast<uint8_t>((field_id << 4) | 12)); // type 12 = struct
}

void ParquetWriter::WriteThriftStop(std::vector<uint8_t> &buf) {
    buf.push_back(0); // stop field
}

// ============================================================================
// Parquet Writer
// ============================================================================

ParquetWriter::ParquetWriter(const std::string &path,
                             const std::vector<std::string> &column_names,
                             const std::vector<LogicalType> &column_types)
    : path_(path), column_names_(column_names), column_types_(column_types) {
    file_.open(path, std::ios::binary);
    if (!file_.is_open())
        throw IOException(ErrorCode::FILE_WRITE_ERROR, "Cannot create Parquet file: " + path);

    // Write magic.
    file_.write(PARQUET_MAGIC, 4);
    meta_.column_names = column_names;
    meta_.column_types = column_types;
}

ParquetWriter::~ParquetWriter() {
    if (!finished_) Finish();
}

void ParquetWriter::WriteColumnChunk(const std::vector<Value> &values,
                                      const LogicalType &type,
                                      ParquetColumnMeta &meta) {
    auto ptype = LogicalToParquetType(type);
    meta.parquet_type = ptype;
    meta.slothdb_type = type;
    meta.num_values = static_cast<int64_t>(values.size());
    meta.data_offset = current_offset_;

    // Write PLAIN-encoded data.
    // Format: [page_header][data]
    // Simplified: we write raw values without a proper page header.
    // Each value: [4-byte is_null flag][data]
    std::vector<uint8_t> data;

    Value min_val, max_val;
    bool has_min = false;

    for (auto &v : values) {
        if (v.IsNull()) {
            uint32_t null_marker = 0xFFFFFFFF;
            auto *p = reinterpret_cast<uint8_t *>(&null_marker);
            data.insert(data.end(), p, p + 4);
            continue;
        }

        uint32_t not_null = 0;
        auto *np = reinterpret_cast<uint8_t *>(&not_null);
        data.insert(data.end(), np, np + 4);

        // Track stats.
        if (!has_min || v < min_val) min_val = v;
        if (!has_min || max_val < v) max_val = v;
        has_min = true;

        switch (ptype) {
        case ParquetType::BOOLEAN: {
            uint8_t b = v.GetValue<bool>() ? 1 : 0;
            data.push_back(b);
            break;
        }
        case ParquetType::INT32: {
            int32_t val = v.GetValue<int32_t>();
            auto *p = reinterpret_cast<uint8_t *>(&val);
            data.insert(data.end(), p, p + 4);
            break;
        }
        case ParquetType::INT64: {
            int64_t val = (v.type().id() == LogicalTypeId::INTEGER)
                ? static_cast<int64_t>(v.GetValue<int32_t>()) : v.GetValue<int64_t>();
            auto *p = reinterpret_cast<uint8_t *>(&val);
            data.insert(data.end(), p, p + 8);
            break;
        }
        case ParquetType::FLOAT: {
            float val = v.GetValue<float>();
            auto *p = reinterpret_cast<uint8_t *>(&val);
            data.insert(data.end(), p, p + 4);
            break;
        }
        case ParquetType::DOUBLE: {
            double val = (v.type().id() == LogicalTypeId::INTEGER)
                ? static_cast<double>(v.GetValue<int32_t>())
                : (v.type().id() == LogicalTypeId::FLOAT)
                    ? static_cast<double>(v.GetValue<float>())
                    : v.GetValue<double>();
            auto *p = reinterpret_cast<uint8_t *>(&val);
            data.insert(data.end(), p, p + 8);
            break;
        }
        case ParquetType::BYTE_ARRAY: {
            auto s = v.ToString();
            uint32_t len = static_cast<uint32_t>(s.size());
            auto *lp = reinterpret_cast<uint8_t *>(&len);
            data.insert(data.end(), lp, lp + 4);
            data.insert(data.end(), s.begin(), s.end());
            break;
        }
        }
    }

    file_.write(reinterpret_cast<const char *>(data.data()), data.size());
    meta.data_size = static_cast<int64_t>(data.size());
    meta.has_stats = has_min;
    meta.min_value = min_val;
    meta.max_value = max_val;
    current_offset_ += meta.data_size;
}

void ParquetWriter::WriteRowGroup(const std::vector<std::vector<Value>> &rows) {
    if (rows.empty()) return;

    ParquetRowGroup rg;
    rg.num_rows = static_cast<int64_t>(rows.size());

    // Transpose rows to columns.
    idx_t num_cols = column_types_.size();
    for (idx_t col = 0; col < num_cols; col++) {
        std::vector<Value> column_data;
        for (auto &row : rows) {
            column_data.push_back(col < row.size() ? row[col] : Value());
        }

        ParquetColumnMeta col_meta;
        col_meta.name = column_names_[col];
        WriteColumnChunk(column_data, column_types_[col], col_meta);
        rg.columns.push_back(std::move(col_meta));
    }

    meta_.num_rows += rg.num_rows;
    meta_.row_groups.push_back(std::move(rg));
}

void ParquetWriter::Finish() {
    if (finished_) return;
    finished_ = true;

    // Write footer: simplified metadata as binary.
    // Format: [num_row_groups(4)][for each rg: num_rows(8), num_cols(4),
    //          for each col: name_len(4), name, type(4), offset(8), size(8),
    //          has_stats(1), min_str_len(4), min_str, max_str_len(4), max_str]
    // Then: [footer_size(4)][PAR1]

    (void)current_offset_;
    std::vector<uint8_t> footer;

    uint32_t num_rg = static_cast<uint32_t>(meta_.row_groups.size());
    auto *rp = reinterpret_cast<uint8_t *>(&num_rg);
    footer.insert(footer.end(), rp, rp + 4);

    // Total rows.
    auto *trp = reinterpret_cast<uint8_t *>(&meta_.num_rows);
    footer.insert(footer.end(), trp, trp + 8);

    // Number of columns.
    uint32_t num_cols = static_cast<uint32_t>(column_names_.size());
    auto *ncp = reinterpret_cast<uint8_t *>(&num_cols);
    footer.insert(footer.end(), ncp, ncp + 4);

    // Column names and types.
    for (idx_t c = 0; c < num_cols; c++) {
        uint32_t name_len = static_cast<uint32_t>(column_names_[c].size());
        auto *nlp = reinterpret_cast<uint8_t *>(&name_len);
        footer.insert(footer.end(), nlp, nlp + 4);
        footer.insert(footer.end(), column_names_[c].begin(), column_names_[c].end());

        int32_t type_id = static_cast<int32_t>(LogicalToParquetType(column_types_[c]));
        auto *tip = reinterpret_cast<uint8_t *>(&type_id);
        footer.insert(footer.end(), tip, tip + 4);
    }

    // Row group metadata.
    for (auto &rg : meta_.row_groups) {
        auto *nrp = reinterpret_cast<uint8_t *>(&rg.num_rows);
        footer.insert(footer.end(), nrp, nrp + 8);

        for (auto &col : rg.columns) {
            auto *dop = reinterpret_cast<uint8_t *>(&col.data_offset);
            footer.insert(footer.end(), dop, dop + 8);
            auto *dsp = reinterpret_cast<uint8_t *>(&col.data_size);
            footer.insert(footer.end(), dsp, dsp + 8);

            footer.push_back(col.has_stats ? 1 : 0);
            if (col.has_stats) {
                auto min_s = col.min_value.ToString();
                auto max_s = col.max_value.ToString();
                uint32_t min_len = static_cast<uint32_t>(min_s.size());
                uint32_t max_len = static_cast<uint32_t>(max_s.size());
                auto *mlp = reinterpret_cast<uint8_t *>(&min_len);
                footer.insert(footer.end(), mlp, mlp + 4);
                footer.insert(footer.end(), min_s.begin(), min_s.end());
                auto *mxp = reinterpret_cast<uint8_t *>(&max_len);
                footer.insert(footer.end(), mxp, mxp + 4);
                footer.insert(footer.end(), max_s.begin(), max_s.end());
            }
        }
    }

    file_.write(reinterpret_cast<const char *>(footer.data()), footer.size());
    uint32_t footer_size = static_cast<uint32_t>(footer.size());
    file_.write(reinterpret_cast<const char *>(&footer_size), 4);
    file_.write(PARQUET_MAGIC, 4);
    file_.close();
}

// ============================================================================
// Parquet Reader
// ============================================================================

ParquetReader::ParquetReader(const std::string &path) : path_(path) {
    // Map (or fread-small) the file. Gives us a stable read-only byte buffer
    // all decode paths share - no per-column file opens.
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Parquet: " + path_);

    LARGE_INTEGER fsz;
    GetFileSizeEx(hFile, &fsz);
    file_size_ = static_cast<size_t>(fsz.QuadPart);
    if (file_size_ < 12) {
        CloseHandle(hFile);
        throw IOException(ErrorCode::CORRUPT_DATA, "File too small for Parquet");
    }
    if (file_size_ < 4 * 1024 * 1024) {
        auto *buf = static_cast<uint8_t *>(std::malloc(file_size_));
        if (!buf) { CloseHandle(hFile); throw InternalException("Out of memory"); }
        DWORD got = 0;
        ReadFile(hFile, buf, static_cast<DWORD>(file_size_), &got, NULL);
        CloseHandle(hFile);
        file_data_ = buf;
        owns_buffer_ = true;
    } else {
        HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        CloseHandle(hFile);
        if (!hMap) throw IOException(ErrorCode::FILE_READ_ERROR, "Cannot mmap Parquet: " + path_);
        auto *p = static_cast<const uint8_t *>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
        if (!p) { CloseHandle(hMap); throw IOException(ErrorCode::FILE_READ_ERROR, "MapViewOfFile failed: " + path_); }
        file_data_ = p;
        mmap_handle_ = hMap;
        owns_mmap_ = true;
    }
#else
    int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Parquet: " + path_);
    struct stat st; ::fstat(fd, &st);
    file_size_ = static_cast<size_t>(st.st_size);
    if (file_size_ < 12) { ::close(fd); throw IOException(ErrorCode::CORRUPT_DATA, "File too small for Parquet"); }
    auto *p = static_cast<const uint8_t *>(::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd, 0));
    ::close(fd);
    if (p == MAP_FAILED) throw IOException(ErrorCode::FILE_READ_ERROR, "mmap failed: " + path_);
    file_data_ = p;
    owns_mmap_ = true;
#endif

    ReadMetadata();
}

ParquetReader::~ParquetReader() {
    if (owns_buffer_) {
        std::free(const_cast<uint8_t *>(file_data_));
    } else if (owns_mmap_) {
#ifdef _WIN32
        if (file_data_) UnmapViewOfFile(const_cast<uint8_t *>(file_data_));
        if (mmap_handle_) CloseHandle(static_cast<HANDLE>(mmap_handle_));
#else
        if (file_data_) ::munmap(const_cast<uint8_t *>(file_data_), file_size_);
#endif
    }
}

void ParquetReader::ReadMetadata() {
    if (file_size_ < 12)
        throw IOException(ErrorCode::CORRUPT_DATA, "File too small for Parquet");

    // Magic at start.
    if (std::memcmp(file_data_, PARQUET_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not a Parquet file (bad magic)");

    // Footer size and end magic.
    uint32_t footer_size;
    std::memcpy(&footer_size, file_data_ + file_size_ - 8, 4);
    if (std::memcmp(file_data_ + file_size_ - 4, PARQUET_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not a Parquet file (bad end magic)");

    if (footer_size > file_size_ - 8)
        throw IOException(ErrorCode::CORRUPT_DATA, "Parquet footer size exceeds file");
    if (footer_size < 16)
        throw IOException(ErrorCode::CORRUPT_DATA, "Parquet footer too small");

    size_t footer_offset = file_size_ - 8 - footer_size;
    // Footer is contiguous in the mapped region - reference it directly.
    const uint8_t *footer_ptr = file_data_ + footer_offset;
    // Some downstream code expects a std::vector<uint8_t>; give it one backed
    // by the mapped bytes (copy is tiny - typical footers are a few KB).
    std::vector<uint8_t> footer(footer_ptr, footer_ptr + footer_size);

    // Try standard Parquet (Thrift FileMetaData). Legacy custom format
    // stores the column count as the very first 4 raw bytes; Thrift starts
    // with a field header byte instead, so we can usually disambiguate by
    // attempting to parse as Thrift first.
    {
        ParqFileMeta pmeta;
        try {
            if (ParseFileMeta(footer.data(), footer.size(), pmeta) &&
                !pmeta.schema.empty() && !pmeta.row_groups.empty()) {
                // Build our internal meta_ from the standard meta.
                // Skip the root schema group (index 0).
                std::vector<idx_t> leaf_indices;
                for (size_t i = 1; i < pmeta.schema.size(); i++) {
                    if (pmeta.schema[i].num_children == 0) leaf_indices.push_back(i);
                }
                meta_.num_rows = pmeta.num_rows;
                meta_.column_names.clear();
                meta_.column_types.clear();
                for (auto i : leaf_indices) {
                    meta_.column_names.push_back(pmeta.schema[i].name);
                    auto t = pmeta.schema[i].type;
                    LogicalType lt = LogicalType::VARCHAR();
                    switch (t) {
                    case 0: lt = LogicalType::BOOLEAN(); break;
                    case 1: lt = LogicalType::INTEGER(); break;
                    case 2: lt = LogicalType::BIGINT(); break;
                    case 4: lt = LogicalType::FLOAT(); break;
                    case 5: lt = LogicalType::DOUBLE(); break;
                    case 6: lt = LogicalType::VARCHAR(); break;
                    default: lt = LogicalType::VARCHAR(); break;
                    }
                    meta_.column_types.push_back(lt);
                    meta_.column_repetition.push_back(pmeta.schema[i].repetition_type);
                }
                meta_.row_groups.clear();
                // Decode raw min/max bytes from a Parquet column-stats blob
                // into a typed Value matching the column's logical type.
                // Returns true on success (stats reconstructed); false if
                // the type isn't supported for pushdown — caller should
                // mark has_stats=false.
                auto decode_stat = [](const std::string &bytes,
                                       LogicalTypeId type_id,
                                       Value &out) -> bool {
                    switch (type_id) {
                    case LogicalTypeId::BOOLEAN:
                        if (bytes.size() < 1) return false;
                        out = Value::BOOLEAN(bytes[0] != 0);
                        return true;
                    case LogicalTypeId::INTEGER: {
                        if (bytes.size() < 4) return false;
                        int32_t v;
                        std::memcpy(&v, bytes.data(), 4);
                        out = Value::INTEGER(v);
                        return true;
                    }
                    case LogicalTypeId::BIGINT:
                    case LogicalTypeId::DATE:
                    case LogicalTypeId::TIMESTAMP:
                    case LogicalTypeId::TIME: {
                        if (bytes.size() < 8) return false;
                        int64_t v;
                        std::memcpy(&v, bytes.data(), 8);
                        out = Value::BIGINT(v);
                        return true;
                    }
                    case LogicalTypeId::FLOAT: {
                        if (bytes.size() < 4) return false;
                        float v;
                        std::memcpy(&v, bytes.data(), 4);
                        out = Value::DOUBLE(v);
                        return true;
                    }
                    case LogicalTypeId::DOUBLE: {
                        if (bytes.size() < 8) return false;
                        double v;
                        std::memcpy(&v, bytes.data(), 8);
                        out = Value::DOUBLE(v);
                        return true;
                    }
                    case LogicalTypeId::VARCHAR:
                        out = Value::VARCHAR(bytes);
                        return true;
                    default:
                        return false;
                    }
                };
                for (auto &rg_in : pmeta.row_groups) {
                    ParquetRowGroup rg;
                    rg.num_rows = rg_in.num_rows;
                    for (size_t c = 0; c < rg_in.columns.size(); c++) {
                        auto &col_in = rg_in.columns[c];
                        ParquetColumnMeta col;
                        col.name = c < meta_.column_names.size() ? meta_.column_names[c] : col_in.name;
                        col.parquet_type = static_cast<ParquetType>(col_in.parquet_type);
                        col.slothdb_type = c < meta_.column_types.size() ? meta_.column_types[c] : LogicalType::VARCHAR();
                        col.num_values = col_in.num_values;
                        col.data_offset = col_in.data_page_offset;
                        col.data_size = col_in.total_compressed_size;
                        col.codec = col_in.codec;
                        col.total_uncompressed_size = col_in.total_uncompressed_size;
                        col.total_compressed_size = col_in.total_compressed_size;
                        col.dict_page_offset = col_in.dict_page_offset;
                        col.repetition_type = c < meta_.column_repetition.size()
                            ? meta_.column_repetition[c] : 1;
                        // Carry stats over from the Thrift parser (col_in.min_bytes
                        // / max_bytes) by decoding raw bytes into typed Values.
                        // Without this, RowGroupMightMatch always returned true
                        // and zone-map pruning was a no-op.
                        if (col_in.has_stats) {
                            Value min_v, max_v;
                            if (decode_stat(col_in.min_bytes, col.slothdb_type.id(), min_v) &&
                                decode_stat(col_in.max_bytes, col.slothdb_type.id(), max_v)) {
                                col.has_stats = true;
                                col.min_value = std::move(min_v);
                                col.max_value = std::move(max_v);
                            }
                        }
                        rg.columns.push_back(std::move(col));
                    }
                    meta_.row_groups.push_back(std::move(rg));
                }
                is_standard_parquet_ = true;
                meta_read_ = true;
                return;
            }
        } catch (...) {
            // Fall through to legacy format.
        }
    }
    // Fall through: legacy custom format (existing parser below).

    // Bounds-checked read helper.
    auto safe_read = [&](size_t &p, void *dst, size_t n) {
        if (p + n > footer.size())
            throw IOException(ErrorCode::CORRUPT_DATA, "Parquet footer truncated");
        std::memcpy(dst, &footer[p], n);
        p += n;
    };

    // Parse footer.
    size_t pos = 0;
    uint32_t num_rg;
    safe_read(pos, &num_rg, 4);

    safe_read(pos, &meta_.num_rows, 8);

    uint32_t num_cols;
    std::memcpy(&num_cols, &footer[pos], 4); pos += 4;

    for (uint32_t c = 0; c < num_cols; c++) {
        uint32_t name_len;
        std::memcpy(&name_len, &footer[pos], 4); pos += 4;
        meta_.column_names.emplace_back(reinterpret_cast<const char *>(&footer[pos]), name_len);
        pos += name_len;

        int32_t type_id;
        std::memcpy(&type_id, &footer[pos], 4); pos += 4;
        meta_.column_types.push_back(ParquetToLogicalType(static_cast<ParquetType>(type_id)));
    }

    for (uint32_t r = 0; r < num_rg; r++) {
        ParquetRowGroup rg;
        std::memcpy(&rg.num_rows, &footer[pos], 8); pos += 8;

        for (uint32_t c = 0; c < num_cols; c++) {
            ParquetColumnMeta col;
            col.name = meta_.column_names[c];
            col.slothdb_type = meta_.column_types[c];
            col.parquet_type = LogicalToParquetType(meta_.column_types[c]);

            std::memcpy(&col.data_offset, &footer[pos], 8); pos += 8;
            std::memcpy(&col.data_size, &footer[pos], 8); pos += 8;

            col.has_stats = footer[pos++] != 0;
            if (col.has_stats) {
                uint32_t min_len;
                std::memcpy(&min_len, &footer[pos], 4); pos += 4;
                auto min_str = std::string(reinterpret_cast<const char *>(&footer[pos]), min_len);
                pos += min_len;
                uint32_t max_len;
                std::memcpy(&max_len, &footer[pos], 4); pos += 4;
                auto max_str = std::string(reinterpret_cast<const char *>(&footer[pos]), max_len);
                pos += max_len;

                // Reconstruct min/max values.
                auto &type = meta_.column_types[c];
                try {
                    if (type.id() == LogicalTypeId::INTEGER) {
                        col.min_value = Value::INTEGER(std::stoi(min_str));
                        col.max_value = Value::INTEGER(std::stoi(max_str));
                    } else if (type.id() == LogicalTypeId::BIGINT) {
                        col.min_value = Value::BIGINT(std::stoll(min_str));
                        col.max_value = Value::BIGINT(std::stoll(max_str));
                    } else if (type.id() == LogicalTypeId::DOUBLE) {
                        col.min_value = Value::DOUBLE(std::stod(min_str));
                        col.max_value = Value::DOUBLE(std::stod(max_str));
                    } else {
                        col.min_value = Value::VARCHAR(min_str);
                        col.max_value = Value::VARCHAR(max_str);
                    }
                } catch (...) {
                    col.has_stats = false;
                }
            }

            col.num_values = rg.num_rows;
            rg.columns.push_back(std::move(col));
        }
        meta_.row_groups.push_back(std::move(rg));
    }

    meta_read_ = true;
}

// Legacy (custom-format) column chunk reader - reads from the mapped buffer.
static std::vector<Value> ReadColumnChunkLegacy(const uint8_t *file_base, size_t file_size,
                                                const ParquetColumnMeta &meta) {
    if ((size_t)meta.data_offset + (size_t)meta.data_size > file_size) return {};
    const uint8_t *data = file_base + meta.data_offset;
    size_t data_size = (size_t)meta.data_size;

    std::vector<Value> values;
    size_t pos = 0;

    for (int64_t i = 0; i < meta.num_values && pos < data_size; i++) {
        uint32_t null_marker;
        std::memcpy(&null_marker, &data[pos], 4); pos += 4;

        if (null_marker == 0xFFFFFFFF) {
            values.push_back(Value());
            continue;
        }

        switch (meta.parquet_type) {
        case ParquetType::BOOLEAN: values.push_back(Value::BOOLEAN(data[pos++] != 0)); break;
        case ParquetType::INT32: {
            int32_t val; std::memcpy(&val, &data[pos], 4); pos += 4;
            values.push_back(Value::INTEGER(val)); break;
        }
        case ParquetType::INT64: {
            int64_t val; std::memcpy(&val, &data[pos], 8); pos += 8;
            values.push_back(Value::BIGINT(val)); break;
        }
        case ParquetType::FLOAT: {
            float val; std::memcpy(&val, &data[pos], 4); pos += 4;
            values.push_back(Value::FLOAT(val)); break;
        }
        case ParquetType::DOUBLE: {
            double val; std::memcpy(&val, &data[pos], 8); pos += 8;
            values.push_back(Value::DOUBLE(val)); break;
        }
        case ParquetType::BYTE_ARRAY: {
            uint32_t len; std::memcpy(&len, &data[pos], 4); pos += 4;
            values.push_back(Value::VARCHAR(
                std::string(reinterpret_cast<const char *>(&data[pos]), len)));
            pos += len; break;
        }
        }
    }
    return values;
}

// Decode PLAIN-encoded values from a buffer into `out`.
static void DecodePlain(const uint8_t *data, size_t size, ParquetType ptype,
                        int32_t num_values, std::vector<Value> &out) {
    size_t pos = 0;
    for (int32_t i = 0; i < num_values && pos <= size; i++) {
        switch (ptype) {
        case ParquetType::BOOLEAN: {
            // Bit-packed. byte index = i/8, bit = i%8.
            uint8_t b = data[i / 8];
            out.push_back(Value::BOOLEAN(((b >> (i % 8)) & 1) != 0));
            if ((i & 7) == 7) pos = (size_t)(i / 8) + 1;
            break;
        }
        case ParquetType::INT32: {
            if (pos + 4 > size) return;
            int32_t v; std::memcpy(&v, &data[pos], 4); pos += 4;
            out.push_back(Value::INTEGER(v));
            break;
        }
        case ParquetType::INT64: {
            if (pos + 8 > size) return;
            int64_t v; std::memcpy(&v, &data[pos], 8); pos += 8;
            out.push_back(Value::BIGINT(v));
            break;
        }
        case ParquetType::FLOAT: {
            if (pos + 4 > size) return;
            float v; std::memcpy(&v, &data[pos], 4); pos += 4;
            out.push_back(Value::FLOAT(v));
            break;
        }
        case ParquetType::DOUBLE: {
            if (pos + 8 > size) return;
            double v; std::memcpy(&v, &data[pos], 8); pos += 8;
            out.push_back(Value::DOUBLE(v));
            break;
        }
        case ParquetType::BYTE_ARRAY: {
            if (pos + 4 > size) return;
            uint32_t len = ReadLEU32(&data[pos]); pos += 4;
            if (pos + len > size) return;
            out.push_back(Value::VARCHAR(std::string(reinterpret_cast<const char*>(&data[pos]), len)));
            pos += len;
            break;
        }
        }
    }
}

// Read one page from the given file offset. Returns bytes consumed.
// `body_ptr` is set to point at the (compressed) page body inside `file_base`
// - no copy. Caller must not mutate the bytes.
static size_t ReadOnePageMapped(const uint8_t *file_base, size_t file_size,
                                int64_t offset, ParqPageHeader &hdr,
                                const uint8_t *&body_ptr, size_t &body_size) {
    if (offset < 0 || (size_t)offset >= file_size) { body_ptr = nullptr; body_size = 0; return 0; }
    const uint8_t *p = file_base + offset;
    size_t remaining = file_size - (size_t)offset;
    size_t consumed = ParsePageHeader(p, remaining, hdr);
    body_ptr = p + consumed;
    body_size = (size_t)hdr.compressed_page_size;
    return consumed + (size_t)hdr.compressed_page_size;
}

// Decode a DataPage (V1 or V2) into `out` values. Handles PLAIN and PLAIN_DICTIONARY.
// For flat schemas with nullable columns, def_level bit_width is 1.
// `is_required`: REQUIRED column has no def_levels - skip parsing them so we
// don't consume the value stream as if it were RLE-encoded levels.
static void DecodeDataPage(const ParqPageHeader &hdr, const uint8_t *data, size_t size,
                           ParquetType ptype, const std::vector<Value> &dict,
                           std::vector<Value> &out, bool is_required = false) {
    const uint8_t *p = data;
    size_t remaining = size;

    // V1 pages start with RLE-packed def/rep levels prefixed by 4-byte little-endian length.
    // Flat, non-null schemas have no levels. Assume flat with possible nulls (max_def=1).
    std::vector<uint8_t> def_mask; // one byte per value (0 = null, 1 = present)
    int32_t n_values = hdr.num_values;

    if (is_required) {
        // No rep_levels, no def_levels.
    } else if (hdr.type == 0) {
        // DATA_PAGE V1: 4-byte length, then RLE-packed def levels (bit_width=1).
        if (remaining >= 4) {
            uint32_t def_len = ReadLEU32(p);
            p += 4; remaining -= 4;
            if (def_len <= remaining) {
                RleDecoder rd(p, def_len, 1);
                def_mask.resize(n_values);
                for (int32_t i = 0; i < n_values; i++) {
                    uint32_t v = 0;
                    if (!rd.Next(v)) { def_mask[i] = 1; continue; }
                    def_mask[i] = static_cast<uint8_t>(v);
                }
                p += def_len; remaining -= def_len;
            }
        }
    } else if (hdr.type == 3) {
        // DATA_PAGE V2: def/rep levels come first, uncompressed, with explicit lengths.
        if (hdr.rep_levels_byte_length > 0) {
            if ((size_t)hdr.rep_levels_byte_length > remaining) return;
            p += hdr.rep_levels_byte_length;
            remaining -= hdr.rep_levels_byte_length;
        }
        if (hdr.def_levels_byte_length > 0) {
            if ((size_t)hdr.def_levels_byte_length > remaining) return;
            // For V2, def levels are RLE without the 4-byte length prefix.
            RleDecoder rd(p, hdr.def_levels_byte_length, 1);
            def_mask.resize(n_values);
            for (int32_t i = 0; i < n_values; i++) {
                uint32_t v = 0;
                if (!rd.Next(v)) { def_mask[i] = 1; continue; }
                def_mask[i] = static_cast<uint8_t>(v);
            }
            p += hdr.def_levels_byte_length;
            remaining -= hdr.def_levels_byte_length;
        }
    }

    // Encoding: 0=PLAIN, 2=PLAIN_DICTIONARY, 8=RLE_DICTIONARY.
    bool dict_enc = (hdr.encoding == 2 || hdr.encoding == 8);
    if (!dict_enc) {
        // PLAIN: decode N non-null values consecutively.
        int32_t non_null = 0;
        if (def_mask.empty()) non_null = n_values;
        else for (int32_t i = 0; i < n_values; i++) if (def_mask[i]) non_null++;
        std::vector<Value> plain_vals;
        plain_vals.reserve(non_null);
        DecodePlain(p, remaining, ptype, non_null, plain_vals);
        // Scatter into `out` based on def_mask.
        if (def_mask.empty()) {
            for (auto &v : plain_vals) out.push_back(std::move(v));
        } else {
            size_t j = 0;
            for (int32_t i = 0; i < n_values; i++) {
                if (def_mask[i]) {
                    out.push_back(j < plain_vals.size() ? plain_vals[j++] : Value());
                } else {
                    out.push_back(Value());
                }
            }
        }
    } else {
        // Dictionary-encoded data page.
        // First byte = bit_width, then RLE/bit-packed hybrid indices.
        if (remaining < 1) return;
        int bit_width = p[0];
        p++; remaining--;
        int32_t non_null = 0;
        if (def_mask.empty()) non_null = n_values;
        else for (int32_t i = 0; i < n_values; i++) if (def_mask[i]) non_null++;
        RleDecoder rd(p, remaining, bit_width);
        size_t dj = 0;
        if (def_mask.empty()) {
            for (int32_t i = 0; i < n_values; i++) {
                uint32_t idx = 0;
                if (!rd.Next(idx)) { out.push_back(Value()); continue; }
                out.push_back(idx < dict.size() ? dict[idx] : Value());
                (void)dj;
            }
        } else {
            for (int32_t i = 0; i < n_values; i++) {
                if (!def_mask[i]) { out.push_back(Value()); continue; }
                uint32_t idx = 0;
                if (!rd.Next(idx)) { out.push_back(Value()); continue; }
                out.push_back(idx < dict.size() ? dict[idx] : Value());
            }
        }
    }
}

// Standard-Parquet column chunk reader - reads from the mapped buffer.
static std::vector<Value> ReadColumnChunkStd(const uint8_t *file_base, size_t file_size,
                                             const ParquetColumnMeta &meta) {
    std::vector<Value> values;
    values.reserve(meta.num_values);

    // 1. Dictionary page (optional).
    std::vector<Value> dict;
    int64_t cur_offset;
    if (meta.dict_page_offset >= 0) {
        ParqPageHeader hdr;
        const uint8_t *body; size_t body_size;
        size_t consumed = ReadOnePageMapped(file_base, file_size, meta.dict_page_offset,
                                            hdr, body, body_size);
        if (hdr.type == 2) {
            std::vector<uint8_t> decompressed;
            if (meta.codec != 0) {
                if (!DecompressPage(meta.codec, body, body_size,
                                     static_cast<size_t>(hdr.uncompressed_page_size), decompressed)) {
                    throw IOException(ErrorCode::CORRUPT_DATA,
                        std::string("Unsupported Parquet compression codec: ") +
                        CodecName(meta.codec) + " (" + std::to_string(meta.codec) +
                        "). Supported: UNCOMPRESSED, SNAPPY, GZIP, ZSTD.");
                }
                body = decompressed.data();
                body_size = decompressed.size();
            }
            DecodePlain(body, body_size, meta.parquet_type, hdr.dict_num_values, dict);
        }
        cur_offset = meta.dict_page_offset + (int64_t)consumed;
        if (cur_offset < meta.data_offset) cur_offset = meta.data_offset;
    } else {
        cur_offset = meta.data_offset;
    }

    int64_t remaining_rows = meta.num_values;
    int64_t total_chunk_end = meta.data_offset + meta.total_compressed_size;

    while (remaining_rows > 0 && cur_offset < (int64_t)file_size) {
        ParqPageHeader hdr;
        const uint8_t *body; size_t body_size;
        size_t consumed = ReadOnePageMapped(file_base, file_size, cur_offset, hdr, body, body_size);
        cur_offset += (int64_t)consumed;

        if (hdr.type == 2) continue; // another dict page, skip

        std::vector<uint8_t> decompressed;
        bool compressed = (hdr.type == 3) ? hdr.is_compressed : (meta.codec != 0);
        if (compressed && meta.codec != 0) {
            if (!DecompressPage(meta.codec, body, body_size,
                                 static_cast<size_t>(hdr.uncompressed_page_size), decompressed)) {
                throw IOException(ErrorCode::CORRUPT_DATA,
                    std::string("Parquet decompression failed: codec ") +
                    CodecName(meta.codec) + " (" + std::to_string(meta.codec) +
                    "). Supported: UNCOMPRESSED, SNAPPY, GZIP, ZSTD.");
            }
            body = decompressed.data();
            body_size = decompressed.size();
        }

        DecodeDataPage(hdr, body, body_size, meta.parquet_type, dict, values,
                       meta.repetition_type == 0);
        remaining_rows -= hdr.num_values;

        if (cur_offset >= total_chunk_end) break;
    }

    return values;
}

std::vector<Value> ParquetReader::ReadColumnChunk(const ParquetColumnMeta &meta) {
    if (is_standard_parquet_) return ReadColumnChunkStd(file_data_, file_size_, meta);
    return ReadColumnChunkLegacy(file_data_, file_size_, meta);
}

// ============================================================================
// Native typed decode - writes directly into ParquetColumnData buffers
// without boxing every value in `Value`.
// ============================================================================
namespace {

struct ParquetDict {
    bool present = false;
    int32_t num_values = 0;
    // String dictionary (pointers into ParquetColumnData::str_heap once it's built).
    std::vector<const char *> str_ptr;
    std::vector<uint32_t>     str_len;
    // Numeric / bool dictionary (one of these is used).
    std::vector<int32_t> i32;
    std::vector<int64_t> i64;
    std::vector<float>   f32;
    std::vector<double>  f64;
    std::vector<uint8_t> b8;
};

// Decode PLAIN-encoded numeric data directly into a typed buffer slice.
// `non_null` values are consumed from `data`. Writes into `dst` at positions
// where `def[i]` == 1 (or all positions if def is empty). Returns true on success.
template <class T>
bool DecodePlainNumericInto(const uint8_t *data, size_t size, int32_t n_values,
                            const std::vector<uint8_t> &def, T *dst) {
    size_t pos = 0;
    if (def.empty()) {
        if ((size_t)n_values * sizeof(T) > size) return false;
        std::memcpy(dst, data, (size_t)n_values * sizeof(T));
        return true;
    }
    size_t j = 0;
    for (int32_t i = 0; i < n_values; i++) {
        if (def[i]) {
            if (pos + sizeof(T) > size) return false;
            T v; std::memcpy(&v, data + pos, sizeof(T));
            pos += sizeof(T);
            dst[i] = v;
            j++;
        }
    }
    (void)j;
    return true;
}

// PLAIN BOOLEAN is bit-packed (1 bit per value).
bool DecodePlainBoolInto(const uint8_t *data, size_t size, int32_t n_values,
                         const std::vector<uint8_t> &def, uint8_t *dst) {
    // Count non-null to bound the bit-packed region.
    int32_t non_null = 0;
    if (def.empty()) non_null = n_values;
    else for (int32_t i = 0; i < n_values; i++) if (def[i]) non_null++;
    size_t need_bytes = (size_t)((non_null + 7) / 8);
    if (need_bytes > size) return false;
    int32_t j = 0;
    for (int32_t i = 0; i < n_values; i++) {
        if (!def.empty() && !def[i]) continue;
        uint8_t b = data[j / 8];
        dst[i] = ((b >> (j % 8)) & 1) ? 1 : 0;
        j++;
    }
    return true;
}

// Append PLAIN-encoded VARCHAR data into `heap`, writing string_t entries to
// dst[i] for non-null rows. heap is a pre-reserved buffer (no reallocation).
bool DecodePlainStringInto(const uint8_t *data, size_t size, int32_t n_values,
                           const std::vector<uint8_t> &def, string_t *dst,
                           std::vector<char> &heap) {
    size_t pos = 0;
    for (int32_t i = 0; i < n_values; i++) {
        if (!def.empty() && !def[i]) continue;
        if (pos + 4 > size) return false;
        uint32_t len = ReadLEU32(data + pos); pos += 4;
        if (pos + len > size) return false;
        if (len <= string_t::INLINE_LENGTH) {
            dst[i] = string_t(reinterpret_cast<const char *>(data + pos), len);
        } else {
            // Append into heap. Heap must have enough reserved capacity so the
            // pointer to the just-appended bytes stays valid.
            size_t old_size = heap.size();
            if (old_size + len > heap.capacity()) return false; // heap overflow
            heap.insert(heap.end(), reinterpret_cast<const char *>(data + pos),
                        reinterpret_cast<const char *>(data + pos) + len);
            dst[i] = string_t(heap.data() + old_size, len);
        }
        pos += len;
    }
    return true;
}

// Helper: read def_levels (bit-width 1) and populate def_mask (size n_values,
// byte-per-row, 1 = valid, 0 = null). For DATA_PAGE V1 the levels are prefixed
// by a 4-byte LE length; for V2 they are passed without a length prefix.
// Returns number of bytes consumed from `data`. Sets out_non_null count.
size_t ReadDefLevels(const uint8_t *data, size_t size, int32_t n_values,
                     std::vector<uint8_t> &def_mask, int32_t &out_non_null,
                     bool has_length_prefix, size_t explicit_len) {
    out_non_null = n_values;
    def_mask.clear();
    if (has_length_prefix) {
        if (size < 4) return 0;
        uint32_t def_len = ReadLEU32(data);
        if (def_len + 4 > size) return 0;
        RleDecoder rd(data + 4, def_len, 1);
        def_mask.resize(n_values, 1);
        int32_t nn = 0;
        bool saw_null = false;
        for (int32_t i = 0; i < n_values; i++) {
            uint32_t v = 1;
            rd.Next(v);
            def_mask[i] = (uint8_t)(v ? 1 : 0);
            if (v) nn++; else saw_null = true;
        }
        out_non_null = nn;
        if (!saw_null) def_mask.clear(); // all valid - caller can skip mask work
        return 4 + def_len;
    } else {
        if (explicit_len > size) return 0;
        if (explicit_len == 0) return 0;
        RleDecoder rd(data, explicit_len, 1);
        def_mask.resize(n_values, 1);
        int32_t nn = 0;
        bool saw_null = false;
        for (int32_t i = 0; i < n_values; i++) {
            uint32_t v = 1;
            rd.Next(v);
            def_mask[i] = (uint8_t)(v ? 1 : 0);
            if (v) nn++; else saw_null = true;
        }
        out_non_null = nn;
        if (!saw_null) def_mask.clear();
        return explicit_len;
    }
}

// Decode one data page (V1 or V2) into the typed output buffer starting at
// row_offset. Supports PLAIN and PLAIN_DICTIONARY / RLE_DICTIONARY.
// `skip_str_data`: for VARCHAR dict path, skip `dst[i] = ...` per-row writes
// when the caller will resolve strings via str_dict_values + str_dict_indices.
// `is_required`: when true the column is REQUIRED (max_def_level=0) and the
// page body has no def_levels at all - critical for ClickBench-style files
// where required INT64 cols would otherwise consume the dict-index data
// stream as if it were RLE-encoded def levels (gives bit_width=garbage).
bool DecodeDataPageTyped(const ParqPageHeader &hdr, const uint8_t *data, size_t size,
                         ParquetType ptype, const ParquetDict &dict,
                         ParquetColumnData &out, idx_t row_offset,
                         bool skip_str_data, bool is_required = false) {
    const uint8_t *p = data;
    size_t remaining = size;
    int32_t n_values = hdr.num_values;
    std::vector<uint8_t> def_mask;
    int32_t non_null = n_values;

    if (is_required) {
        // REQUIRED column: no rep_levels, no def_levels. Page body starts
        // directly with values.
    } else if (hdr.type == 0) {
        // DATA_PAGE V1: rep levels then def levels.
        // Per parquet-format spec, when def_level_encoding == RLE the levels
        // are wrapped in a 4-byte LE length prefix. When BIT_PACKED, no
        // prefix. Flat schemas have no rep_levels.
        bool has_prefix = (hdr.def_level_encoding == 3); // 3 = RLE
        size_t consumed = ReadDefLevels(p, remaining, n_values, def_mask,
                                         non_null, has_prefix,
                                         has_prefix ? 0 : remaining);
        if (consumed == 0 && remaining >= 4) {
            // ReadDefLevels returns 0 on failure; treat as all-valid and continue.
            def_mask.clear();
            non_null = n_values;
        } else {
            p += consumed; remaining -= consumed;
        }
    } else if (hdr.type == 3) {
        // DATA_PAGE V2: explicit rep/def byte lengths.
        if (hdr.rep_levels_byte_length > 0) {
            if ((size_t)hdr.rep_levels_byte_length > remaining) return false;
            p += hdr.rep_levels_byte_length;
            remaining -= hdr.rep_levels_byte_length;
        }
        if (hdr.def_levels_byte_length > 0) {
            size_t consumed = ReadDefLevels(p, remaining, n_values, def_mask, non_null,
                                             false, (size_t)hdr.def_levels_byte_length);
            if (consumed == 0) return false;
            p += consumed; remaining -= consumed;
        }
    }

    // Track nulls on the output.
    if (!def_mask.empty()) {
        if (out.all_valid) {
            out.validity.assign(out.count, 1);
            out.all_valid = false;
        }
        for (int32_t i = 0; i < n_values; i++) {
            if (!def_mask[i]) out.validity[row_offset + i] = 0;
        }
    }

    bool dict_enc = (hdr.encoding == 2 || hdr.encoding == 8);
    LogicalTypeId tid = out.type.id();

    if (!dict_enc) {
        // PLAIN decode into typed buffer at [row_offset .. row_offset+n_values).
        switch (tid) {
        case LogicalTypeId::BOOLEAN:
            return DecodePlainBoolInto(p, remaining, n_values, def_mask,
                                       out.bool_data.data() + row_offset);
        case LogicalTypeId::INTEGER:
            return DecodePlainNumericInto<int32_t>(p, remaining, n_values, def_mask,
                                                   out.i32_data.data() + row_offset);
        case LogicalTypeId::BIGINT:
            return DecodePlainNumericInto<int64_t>(p, remaining, n_values, def_mask,
                                                   out.i64_data.data() + row_offset);
        case LogicalTypeId::FLOAT:
            return DecodePlainNumericInto<float>(p, remaining, n_values, def_mask,
                                                 out.f32_data.data() + row_offset);
        case LogicalTypeId::DOUBLE:
            return DecodePlainNumericInto<double>(p, remaining, n_values, def_mask,
                                                  out.f64_data.data() + row_offset);
        case LogicalTypeId::VARCHAR:
            if (ptype != ParquetType::BYTE_ARRAY) return false;
            if (out.str_data_skipped && out.str_data.empty())
                MaterialiseStrDataLazy(out, dict.str_ptr.data(), dict.str_len.data(),
                                       (uint32_t)dict.str_ptr.size(), row_offset);
            return DecodePlainStringInto(p, remaining, n_values, def_mask,
                                         out.str_data.data() + row_offset, *out.str_heap);
        default:
            return false;
        }
    }

    // Dictionary-encoded indices: first byte = bit_width, then RLE/bit-packed indices.
    if (remaining < 1) return false;
    int bit_width = p[0];
    p++; remaining--;
    RleDecoder rd(p, remaining, bit_width);
    auto idx_for = [&](int32_t i) -> uint32_t {
        (void)i; uint32_t v = 0; rd.Next(v); return v;
    };

    switch (tid) {
    case LogicalTypeId::BOOLEAN: {
        auto *dst = out.bool_data.data() + row_offset;
        for (int32_t i = 0; i < n_values; i++) {
            if (!def_mask.empty() && !def_mask[i]) continue;
            uint32_t idx = idx_for(i);
            dst[i] = idx < dict.b8.size() ? dict.b8[idx] : 0;
        }
        return true;
    }
    case LogicalTypeId::INTEGER: {
        auto *dst = out.i32_data.data() + row_offset;
        if (def_mask.empty()) {
            uint32_t buf[256];
            const int32_t *dv = dict.i32.data();
            uint32_t dn = static_cast<uint32_t>(dict.i32.size());
            int32_t left = n_values;
            int32_t off = 0;
            while (left > 0) {
                int want = left < 256 ? left : 256;
                int got = rd.NextBatch(buf, want);
                if (got <= 0) break;
                for (int j = 0; j < got; j++) {
                    uint32_t idx = buf[j];
                    dst[off + j] = idx < dn ? dv[idx] : 0;
                }
                off += got; left -= got;
            }
            return true;
        }
        for (int32_t i = 0; i < n_values; i++) {
            if (!def_mask[i]) continue;
            uint32_t idx = idx_for(i);
            dst[i] = idx < dict.i32.size() ? dict.i32[idx] : 0;
        }
        return true;
    }
    case LogicalTypeId::BIGINT: {
        auto *dst = out.i64_data.data() + row_offset;
        if (def_mask.empty()) {
            uint32_t buf[256];
            const int64_t *dv = dict.i64.data();
            uint32_t dn = static_cast<uint32_t>(dict.i64.size());
            int32_t left = n_values;
            int32_t off = 0;
            while (left > 0) {
                int want = left < 256 ? left : 256;
                int got = rd.NextBatch(buf, want);
                if (got <= 0) break;
                for (int j = 0; j < got; j++) {
                    uint32_t idx = buf[j];
                    dst[off + j] = idx < dn ? dv[idx] : 0;
                }
                off += got; left -= got;
            }
            return true;
        }
        for (int32_t i = 0; i < n_values; i++) {
            if (!def_mask[i]) continue;
            uint32_t idx = idx_for(i);
            dst[i] = idx < dict.i64.size() ? dict.i64[idx] : 0;
        }
        return true;
    }
    case LogicalTypeId::FLOAT: {
        auto *dst = out.f32_data.data() + row_offset;
        for (int32_t i = 0; i < n_values; i++) {
            if (!def_mask.empty() && !def_mask[i]) continue;
            uint32_t idx = idx_for(i);
            dst[i] = idx < dict.f32.size() ? dict.f32[idx] : 0.f;
        }
        return true;
    }
    case LogicalTypeId::DOUBLE: {
        auto *dst = out.f64_data.data() + row_offset;
        for (int32_t i = 0; i < n_values; i++) {
            if (!def_mask.empty() && !def_mask[i]) continue;
            uint32_t idx = idx_for(i);
            dst[i] = idx < dict.f64.size() ? dict.f64[idx] : 0.0;
        }
        return true;
    }
    case LogicalTypeId::VARCHAR: {
        string_t *dst = skip_str_data ? nullptr : (out.str_data.data() + row_offset);
        const bool want_indices = !out.str_dict_indices.empty();
        uint32_t *idx_dst = want_indices ? (out.str_dict_indices.data() + row_offset)
                                          : nullptr;
        for (int32_t i = 0; i < n_values; i++) {
            if (!def_mask.empty() && !def_mask[i]) continue;
            uint32_t idx = idx_for(i);
            if (want_indices) idx_dst[i] = idx;
            if (!dst) continue;
            if (idx >= dict.str_ptr.size()) { dst[i] = string_t(); continue; }
            uint32_t len = dict.str_len[idx];
            dst[i] = string_t(dict.str_ptr[idx], len);
        }
        return true;
    }
    default:
        return false;
    }
}

// Read + decompress a dictionary page into `dict`. For VARCHAR, the dict bytes
// are copied into `heap` (so dict pointers remain stable across the RG).
// Sets `consumed` to the number of bytes the dict page occupies in the file.
bool ReadDictionaryPage(const uint8_t *file_base, size_t file_size, int64_t offset,
                        int32_t codec, ParquetType ptype, ParquetDict &dict,
                        std::vector<char> *heap, size_t &consumed) {
    ParqPageHeader hdr;
    const uint8_t *body; size_t body_size;
    consumed = ReadOnePageMapped(file_base, file_size, offset, hdr, body, body_size);
    if (hdr.type != 2) return false;

    std::vector<uint8_t> decomp;
    if (codec != 0) {
        if (!DecompressPage(codec, body, body_size,
                            (size_t)hdr.uncompressed_page_size, decomp)) return false;
        body = decomp.data(); body_size = decomp.size();
    }

    int32_t ndv = hdr.dict_num_values;
    dict.num_values = ndv;
    dict.present = true;
    size_t pos = 0;

    switch (ptype) {
    case ParquetType::INT32:
        dict.i32.resize(ndv);
        if ((size_t)ndv * 4 > body_size) return false;
        std::memcpy(dict.i32.data(), body, (size_t)ndv * 4);
        return true;
    case ParquetType::INT64:
        dict.i64.resize(ndv);
        if ((size_t)ndv * 8 > body_size) return false;
        std::memcpy(dict.i64.data(), body, (size_t)ndv * 8);
        return true;
    case ParquetType::FLOAT:
        dict.f32.resize(ndv);
        if ((size_t)ndv * 4 > body_size) return false;
        std::memcpy(dict.f32.data(), body, (size_t)ndv * 4);
        return true;
    case ParquetType::DOUBLE:
        dict.f64.resize(ndv);
        if ((size_t)ndv * 8 > body_size) return false;
        std::memcpy(dict.f64.data(), body, (size_t)ndv * 8);
        return true;
    case ParquetType::BOOLEAN: {
        dict.b8.resize(ndv);
        // PLAIN BOOL bit-packed in dict.
        for (int32_t i = 0; i < ndv; i++) {
            if ((size_t)(i / 8) >= body_size) return false;
            dict.b8[i] = ((body[i / 8] >> (i % 8)) & 1) ? 1 : 0;
        }
        return true;
    }
    case ParquetType::BYTE_ARRAY: {
        if (!heap) return false;
        dict.str_ptr.resize(ndv);
        dict.str_len.resize(ndv);
        for (int32_t i = 0; i < ndv; i++) {
            if (pos + 4 > body_size) return false;
            uint32_t len = ReadLEU32(body + pos); pos += 4;
            if (pos + len > body_size) return false;
            if (heap->size() + len > heap->capacity()) return false; // over-reserve bug
            size_t start = heap->size();
            heap->insert(heap->end(), reinterpret_cast<const char *>(body + pos),
                         reinterpret_cast<const char *>(body + pos) + len);
            dict.str_ptr[i] = heap->data() + start;
            dict.str_len[i] = len;
            pos += len;
        }
        return true;
    }
    }
    return false;
}

} // namespace

bool ParquetReader::ReadColumnInto(idx_t rg_idx, idx_t col_idx, ParquetColumnData &out,
                                    bool skip_str_data) {
    // Don't Clear() - reuse any existing capacity from a prior RG. We reset
    // only the control fields; the numeric data vectors are resized below,
    // which is a no-op when sizes match and only zero-fills the delta
    // otherwise. Across 80 RGs this saves ~hundreds of MB of malloc/memset.
    out.count = 0;
    out.validity.clear();
    out.all_valid = true;
    out.decoded = false;
    out.str_dict_encoded = false;
    out.str_dict_indices.clear();
    out.str_dict_values.clear();
    out.str_heap.reset();
    if (!is_standard_parquet_) return false;
    if (rg_idx >= meta_.row_groups.size()) return false;
    auto &rg = meta_.row_groups[rg_idx];
    if (col_idx >= rg.columns.size()) return false;
    auto &cmeta = rg.columns[col_idx];

    // Codecs supported by DecompressPage: UNCOMPRESSED (0), SNAPPY (1),
    // GZIP (2), ZSTD (6). Anything else falls back to the slow path.
    if (cmeta.codec != 0 && cmeta.codec != 1 && cmeta.codec != 2 && cmeta.codec != 6) return false;

    const idx_t total_rows = static_cast<idx_t>(cmeta.num_values);
    out.type = cmeta.slothdb_type;
    out.count = total_rows;
    out.all_valid = true;

    LogicalTypeId tid = cmeta.slothdb_type.id();
    switch (tid) {
    // resize() keeps existing contents up to total_rows and only value-
    // initializes the delta when the vector grows. For persistent per-worker
    // RGWork, successive RGs of the same size pay zero here.
    case LogicalTypeId::BOOLEAN: out.bool_data.resize(total_rows); break;
    case LogicalTypeId::INTEGER: out.i32_data.resize(total_rows); break;
    case LogicalTypeId::BIGINT:  out.i64_data.resize(total_rows); break;
    case LogicalTypeId::FLOAT:   out.f32_data.resize(total_rows); break;
    case LogicalTypeId::DOUBLE:  out.f64_data.resize(total_rows); break;
    case LogicalTypeId::VARCHAR:
        if (!skip_str_data) {
            out.str_data.resize(total_rows);
        } else {
            // Skip-mode: pre-reserve the full buffer so that a mid-RG
            // MaterialiseStrDataLazy resize doesn't reallocate. Callers can
            // safely capture str_data.data() once at RG start; the pointer
            // stays valid even if PLAIN pages later trigger back-fill.
            out.str_data.clear();
            out.str_data.reserve(total_rows);
        }
        out.str_data_skipped = skip_str_data;
        out.str_heap = std::make_shared<std::vector<char>>();
        out.str_heap->reserve((size_t)cmeta.total_uncompressed_size + 64);
        if (cmeta.dict_page_offset >= 0) {
            out.str_dict_indices.resize(total_rows);
            out.str_dict_encoded = true;
        }
        break;
    default:
        return false;
    }

    if (!file_data_) return false;

    // 1) Dictionary page (optional).
    ParquetDict dict;
    int64_t cur_offset;
    if (cmeta.dict_page_offset >= 0) {
        size_t consumed = 0;
        std::vector<char> *heap = (tid == LogicalTypeId::VARCHAR)
                                      ? out.str_heap.get() : nullptr;
        if (!ReadDictionaryPage(file_data_, file_size_, cmeta.dict_page_offset,
                                 cmeta.codec, cmeta.parquet_type, dict, heap, consumed)) {
            return false;
        }
        cur_offset = cmeta.dict_page_offset + (int64_t)consumed;
        if (cur_offset < cmeta.data_offset) cur_offset = cmeta.data_offset;
    } else {
        cur_offset = cmeta.data_offset;
    }

    // 2) Data pages.
    int64_t total_end = cmeta.data_offset + cmeta.total_compressed_size;
    idx_t rows_read = 0;
    // Decompression scratch reused across pages — saves a malloc/free
    // pair per page (~10-20 pages per RG × 80 RGs = lots) on ZSTD/GZIP/SNAPPY
    // codecs. The vector is resized to each page's uncompressed_size; once
    // capacity reaches the largest page, no further allocation happens.
    std::vector<uint8_t> decomp;
    while (rows_read < total_rows && cur_offset < (int64_t)file_size_) {
        ParqPageHeader hdr;
        const uint8_t *body; size_t body_size;
        size_t consumed = ReadOnePageMapped(file_data_, file_size_, cur_offset,
                                            hdr, body, body_size);
        cur_offset += (int64_t)consumed;
        if (hdr.type == 2) continue; // stray dict page

        // Decompress page body if needed.
        bool is_compressed = (hdr.type == 3) ? hdr.is_compressed : (cmeta.codec != 0);
        if (is_compressed && cmeta.codec != 0) {
            if (!DecompressPage(cmeta.codec, body, body_size,
                                (size_t)hdr.uncompressed_page_size, decomp)) return false;
            body = decomp.data(); body_size = decomp.size();
        }

        if (!DecodeDataPageTyped(hdr, body, body_size, cmeta.parquet_type, dict, out,
                                  rows_read, out.str_data_skipped,
                                  cmeta.repetition_type == 0)) {
            return false;
        }
        // A PLAIN (non-dict) data page invalidates the dict-index fast path
        // - rows in that page don't have meaningful dict indices.
        if (tid == LogicalTypeId::VARCHAR && out.str_dict_encoded &&
            hdr.encoding != 2 && hdr.encoding != 8) {
            out.str_dict_encoded = false;
            out.str_dict_indices.clear();
        }
        rows_read += (idx_t)hdr.num_values;
        if (cur_offset >= total_end) break;
    }

    // If every page was dict-encoded, publish the dict values so consumers
    // can resolve indices -> strings without re-parsing the Parquet file.
    if (tid == LogicalTypeId::VARCHAR && out.str_dict_encoded && dict.present) {
        out.str_dict_values.resize(dict.str_ptr.size());
        for (size_t i = 0; i < dict.str_ptr.size(); i++) {
            out.str_dict_values[i] = string_t(dict.str_ptr[i], dict.str_len[i]);
        }
    }

    out.decoded = true;
    return true;
}

std::vector<Value> ParquetReader::ReadColumn(idx_t rg_idx, idx_t col_idx) {
    if (rg_idx >= meta_.row_groups.size()) return {};
    auto &rg = meta_.row_groups[rg_idx];
    if (col_idx >= rg.columns.size()) return {};
    return ReadColumnChunk(rg.columns[col_idx]);
}

// Q4 fast path: walk pages of a BIGINT dict-encoded column, accumulate
// histogram of dict indices, and reduce SUM = sum_idx (count[idx]*dict[idx]).
// Skips materialization of i64_data entirely.
//
// Bails (returns false) if any of:
//  - column type is not BIGINT INT64
//  - column has no dictionary page
//  - any data page is PLAIN (non-dict) — would invalidate the histogram
//  - any null is observed in def_levels
//  - codec is unsupported
// Caller falls back to ReadColumnInto + scalar SUM.
bool ParquetReader::DecodeBigintColumnHistogram(idx_t rg_idx, idx_t col_idx,
                                                int64_t &out_count, double &out_sum) {
    out_count = 0;
    out_sum = 0.0;
    if (!is_standard_parquet_) return false;
    if (!file_data_) return false;
    if (rg_idx >= meta_.row_groups.size()) return false;
    auto &rg = meta_.row_groups[rg_idx];
    if (col_idx >= rg.columns.size()) return false;
    auto &cmeta = rg.columns[col_idx];
    if (cmeta.parquet_type != ParquetType::INT64) return false;
    if (cmeta.slothdb_type.id() != LogicalTypeId::BIGINT) return false;
    if (cmeta.dict_page_offset < 0) return false;
    if (cmeta.codec != 0 && cmeta.codec != 1 && cmeta.codec != 2 && cmeta.codec != 6) return false;

    // 1) Read dict page.
    ParquetDict dict;
    size_t consumed = 0;
    if (!ReadDictionaryPage(file_data_, file_size_, cmeta.dict_page_offset,
                             cmeta.codec, cmeta.parquet_type, dict, nullptr, consumed)) {
        return false;
    }
    if (dict.i64.empty()) return false;

    int64_t cur_offset = cmeta.dict_page_offset + (int64_t)consumed;
    if (cur_offset < cmeta.data_offset) cur_offset = cmeta.data_offset;
    int64_t total_end = cmeta.data_offset + cmeta.total_compressed_size;
    const idx_t total_rows = static_cast<idx_t>(cmeta.num_values);
    const bool is_required = (cmeta.repetition_type == 0);

    // 2) Histogram. Sized to dict.i64.size(). Per-RG max ~31k * 4B = ~125KB.
    //    Stays L2-resident on a single thread; one zero-fill at start.
    std::vector<uint32_t> hist(dict.i64.size(), 0);

    // PLAIN-page carry: pages within an RG can be a mix of dict-encoded and
    // PLAIN. The histogram is only valid for the dict-encoded pages; PLAIN
    // pages contribute directly to (count, sum) and are folded in below.
    int64_t plain_carry_count = 0;
    double  plain_carry_sum = 0.0;

    std::vector<uint8_t> decomp;
    idx_t rows_read = 0;
    while (rows_read < total_rows && cur_offset < (int64_t)file_size_) {
        ParqPageHeader hdr;
        const uint8_t *body; size_t body_size;
        size_t pconsumed = ReadOnePageMapped(file_data_, file_size_, cur_offset,
                                              hdr, body, body_size);
        if (pconsumed == 0) return false;
        cur_offset += (int64_t)pconsumed;
        if (hdr.type == 2) continue; // stray dict page (shouldn't happen mid-column)

        // Decompress page body if needed.
        bool is_compressed = (hdr.type == 3) ? hdr.is_compressed : (cmeta.codec != 0);
        if (is_compressed && cmeta.codec != 0) {
            if (!DecompressPage(cmeta.codec, body, body_size,
                                (size_t)hdr.uncompressed_page_size, decomp)) return false;
            body = decomp.data(); body_size = decomp.size();
        }

        // Skip def_levels if any (and bail on any null - histogram math
        // assumes every row contributes both to count and to sum-via-dict).
        const uint8_t *p = body;
        size_t remaining = body_size;
        int32_t n_values = hdr.num_values;
        int32_t non_null = n_values;
        std::vector<uint8_t> def_mask;

        if (is_required) {
            // No def_levels.
        } else if (hdr.type == 0) {
            // V1 page: rep then def levels (rep absent for flat schemas).
            bool has_prefix = (hdr.def_level_encoding == 3);
            size_t dconsumed = ReadDefLevels(p, remaining, n_values, def_mask,
                                              non_null, has_prefix,
                                              has_prefix ? 0 : remaining);
            if (dconsumed == 0 && remaining >= 4) {
                def_mask.clear();
                non_null = n_values;
            } else {
                p += dconsumed; remaining -= dconsumed;
            }
        } else if (hdr.type == 3) {
            // V2 page.
            if (hdr.rep_levels_byte_length > 0) {
                if ((size_t)hdr.rep_levels_byte_length > remaining) return false;
                p += hdr.rep_levels_byte_length;
                remaining -= hdr.rep_levels_byte_length;
            }
            if (hdr.def_levels_byte_length > 0) {
                size_t dconsumed = ReadDefLevels(p, remaining, n_values, def_mask, non_null,
                                                  false, (size_t)hdr.def_levels_byte_length);
                if (dconsumed == 0) return false;
                p += dconsumed; remaining -= dconsumed;
            }
        }

        // Bail on any null observed (def_mask non-empty after ReadDefLevels
        // means some null was seen). This keeps the histogram math exact.
        if (!def_mask.empty()) return false;
        if (non_null != n_values) return false;

        // Encoding 2 = PLAIN_DICTIONARY, 8 = RLE_DICTIONARY, 0 = PLAIN.
        // For PLAIN pages of INT64 we read 8 bytes per value directly.
        bool dict_enc = (hdr.encoding == 2 || hdr.encoding == 8);
        if (!dict_enc) {
            // PLAIN INT64: walk values directly into running sum + count.
            if (hdr.encoding != 0) return false;
            if ((size_t)n_values * sizeof(int64_t) > remaining) return false;
            const int64_t *vp = reinterpret_cast<const int64_t *>(p);
            // Accumulate via 4-lane double SUM (matches the existing scalar
            // path's precision on this column).
            double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            int32_t i = 0;
            for (; i + 4 <= n_values; i += 4) {
                s0 += static_cast<double>(vp[i]);
                s1 += static_cast<double>(vp[i + 1]);
                s2 += static_cast<double>(vp[i + 2]);
                s3 += static_cast<double>(vp[i + 3]);
            }
            for (; i < n_values; i++) s0 += static_cast<double>(vp[i]);
            plain_carry_count += static_cast<int64_t>(n_values);
            plain_carry_sum   += (s0 + s1) + (s2 + s3);
            rows_read += static_cast<idx_t>(n_values);
            if (cur_offset >= total_end) break;
            continue;
        }

        // Bit-width prefix byte, then RLE/bit-packed indices.
        if (remaining < 1) return false;
        int bit_width = p[0];
        p++; remaining--;
        if (bit_width < 0 || bit_width > 32) return false;

        RleDecoder rd(p, remaining, bit_width);
        uint32_t buf[256];
        const uint32_t dn = static_cast<uint32_t>(dict.i64.size());
        int32_t left = n_values;
        while (left > 0) {
            int want = left < 256 ? left : 256;
            int got = rd.NextBatch(buf, want);
            if (got <= 0) break;
            // Inner loop: histogram bump. The CMOV-style guard `idx < dn`
            // mirrors the standard decode path's bounds check; unbounded
            // index would overflow `hist`. In practice every page has
            // bit_width tight to log2(dict_size), so the branch is never
            // taken — predictor friendly.
            for (int j = 0; j < got; j++) {
                uint32_t idx = buf[j];
                if (idx < dn) hist[idx]++;
            }
            left -= got;
        }

        rows_read += static_cast<idx_t>(n_values);
        if (cur_offset >= total_end) break;
    }

    // 3) Reduce: sum_idx (count[idx] * dict.i64[idx]).
    //    Cast to double to match AggState.sum semantics. ~31k iterations.
    int64_t total_count = 0;
    double total_sum = 0.0;
    const int64_t *dv = dict.i64.data();
    const size_t nd = dict.i64.size();
    for (size_t i = 0; i < nd; i++) {
        uint32_t c = hist[i];
        if (c == 0) continue;
        total_count += static_cast<int64_t>(c);
        total_sum += static_cast<double>(c) * static_cast<double>(dv[i]);
    }

    out_count = total_count + plain_carry_count;
    out_sum = total_sum + plain_carry_sum;
    return true;
}

std::vector<std::vector<Value>> ParquetReader::ReadRowGroup(idx_t rg_idx) {
    if (rg_idx >= meta_.row_groups.size()) return {};

    auto &rg = meta_.row_groups[rg_idx];
    idx_t num_cols = rg.columns.size();

    // Read each column.
    std::vector<std::vector<Value>> columns;
    for (idx_t c = 0; c < num_cols; c++) {
        columns.push_back(ReadColumnChunk(rg.columns[c]));
    }

    // Transpose: columns to rows.
    std::vector<std::vector<Value>> rows;
    for (int64_t r = 0; r < rg.num_rows; r++) {
        std::vector<Value> row;
        for (idx_t c = 0; c < num_cols; c++) {
            row.push_back(r < static_cast<int64_t>(columns[c].size())
                ? columns[c][r] : Value());
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

idx_t ParquetReader::ReadRowGroupChunk(idx_t rg_idx, DataChunk &chunk,
                                        const std::vector<bool> &projection) {
    if (rg_idx >= meta_.row_groups.size()) return 0;
    auto &rg = meta_.row_groups[rg_idx];
    idx_t num_cols = rg.columns.size();

    // Read only projected columns (huge win for column-pruning queries).
    std::vector<std::vector<Value>> columns(num_cols);
    bool prune = !projection.empty();
    for (idx_t c = 0; c < num_cols; c++) {
        if (prune && c < projection.size() && !projection[c]) continue;
        columns[c] = ReadColumnChunk(rg.columns[c]);
    }

    // Write directly into DataChunk vectors - pack VECTOR_SIZE rows per chunk.
    idx_t total_rows = static_cast<idx_t>(rg.num_rows);
    if (total_rows > VECTOR_SIZE) total_rows = VECTOR_SIZE; // first chunk only

    for (idx_t r = 0; r < total_rows; r++) {
        for (idx_t c = 0; c < num_cols; c++) {
            if (prune && c < projection.size() && !projection[c]) {
                chunk.GetVector(c).GetValidity().SetInvalid(r);
                continue;
            }
            if (r < columns[c].size()) {
                chunk.SetValue(c, r, columns[c][r]);
            } else {
                chunk.GetVector(c).GetValidity().SetInvalid(r);
            }
        }
    }
    chunk.SetCardinality(total_rows);
    return total_rows;
}

std::vector<std::vector<Value>> ParquetReader::ReadAll() {
    std::vector<std::vector<Value>> all_rows;
    for (idx_t rg = 0; rg < meta_.row_groups.size(); rg++) {
        auto rows = ReadRowGroup(rg);
        all_rows.insert(all_rows.end(), rows.begin(), rows.end());
    }
    return all_rows;
}

// Coerce `v` to match the type of `target` for cross-type comparisons.
// Value::operator< returns false on type mismatch, so without this
// step `BIGINT(200) < INTEGER(999)` is false and zone-map pruning
// silently fails. Returns false if the coercion isn't safe.
static bool CoerceForCompare(const Value &v, const Value &target, Value &out) {
    if (v.type() == target.type()) { out = v; return true; }
    auto tid = target.type().id();
    // Numeric promotion via bigint / double.
    auto vid = v.type().id();
    auto is_int = [](LogicalTypeId i) {
        return i == LogicalTypeId::TINYINT || i == LogicalTypeId::SMALLINT ||
               i == LogicalTypeId::INTEGER || i == LogicalTypeId::BIGINT;
    };
    auto is_float = [](LogicalTypeId i) {
        return i == LogicalTypeId::FLOAT || i == LogicalTypeId::DOUBLE;
    };
    if (is_int(vid) && is_int(tid)) {
        int64_t i = 0;
        switch (vid) {
        case LogicalTypeId::TINYINT:  i = v.GetValue<int8_t>(); break;
        case LogicalTypeId::SMALLINT: i = v.GetValue<int16_t>(); break;
        case LogicalTypeId::INTEGER:  i = v.GetValue<int32_t>(); break;
        case LogicalTypeId::BIGINT:   i = v.GetValue<int64_t>(); break;
        default: return false;
        }
        switch (tid) {
        case LogicalTypeId::TINYINT:
            if (i < INT8_MIN || i > INT8_MAX) return false;
            out = Value::TINYINT(static_cast<int8_t>(i)); return true;
        case LogicalTypeId::SMALLINT:
            if (i < INT16_MIN || i > INT16_MAX) return false;
            out = Value::SMALLINT(static_cast<int16_t>(i)); return true;
        case LogicalTypeId::INTEGER:
            if (i < INT32_MIN || i > INT32_MAX) return false;
            out = Value::INTEGER(static_cast<int32_t>(i)); return true;
        case LogicalTypeId::BIGINT:
            out = Value::BIGINT(i); return true;
        default: return false;
        }
    }
    if ((is_int(vid) || is_float(vid)) && is_float(tid)) {
        double d = 0.0;
        switch (vid) {
        case LogicalTypeId::TINYINT:  d = v.GetValue<int8_t>(); break;
        case LogicalTypeId::SMALLINT: d = v.GetValue<int16_t>(); break;
        case LogicalTypeId::INTEGER:  d = v.GetValue<int32_t>(); break;
        case LogicalTypeId::BIGINT:   d = static_cast<double>(v.GetValue<int64_t>()); break;
        case LogicalTypeId::FLOAT:    d = v.GetValue<float>(); break;
        case LogicalTypeId::DOUBLE:   d = v.GetValue<double>(); break;
        default: return false;
        }
        if (tid == LogicalTypeId::FLOAT) out = Value::FLOAT(static_cast<float>(d));
        else out = Value::DOUBLE(d);
        return true;
    }
    return false;
}

bool ParquetReader::DictSkipPossible(idx_t rg_idx, idx_t col_idx,
                                      const std::string &op, const Value &val) const {
    if (op != "=" && op != "==") return false;
    if (rg_idx >= meta_.row_groups.size()) return false;
    auto &rg = meta_.row_groups[rg_idx];
    if (col_idx >= rg.columns.size()) return false;
    auto &col = rg.columns[col_idx];
    if (col.parquet_type != ParquetType::INT64) return false;
    if (col.dict_page_offset < 0) return false;
    if (val.type().id() != LogicalTypeId::BIGINT) return false;

    int64_t literal = val.GetValue<int64_t>();
    uint64_t key = (uint64_t)rg_idx << 32 | (uint32_t)col_idx;

    {
        std::lock_guard<std::mutex> lk(dict_cache_mu_);
        auto it = int64_dict_cache_.find(key);
        if (it != int64_dict_cache_.end()) {
            return !std::binary_search(it->second.begin(), it->second.end(), literal);
        }
    }

    ParqPageHeader hdr;
    const uint8_t *body = nullptr; size_t body_size = 0;
    size_t consumed = ReadOnePageMapped(file_data_, file_size_, col.dict_page_offset,
                                         hdr, body, body_size);
    if (consumed == 0 || hdr.type != 2 || body == nullptr) return false;

    std::vector<uint8_t> decompressed;
    if (col.codec != 0) {
        if (!DecompressPage(col.codec, body, body_size,
                            (size_t)hdr.uncompressed_page_size, decompressed)) {
            return false;
        }
        body = decompressed.data();
        body_size = decompressed.size();
    }

    int32_t ndv = hdr.dict_num_values;
    if (ndv <= 0) return false;
    if ((size_t)ndv * sizeof(int64_t) > body_size) return false;

    std::vector<int64_t> dict(ndv);
    std::memcpy(dict.data(), body, (size_t)ndv * sizeof(int64_t));
    std::sort(dict.begin(), dict.end());

    bool present = std::binary_search(dict.begin(), dict.end(), literal);
    if (!present) {
        std::lock_guard<std::mutex> lk(dict_cache_mu_);
        int64_dict_cache_.emplace(key, std::move(dict));
    }
    return !present;
}

bool ParquetReader::RowGroupMightMatch(idx_t rg_idx, idx_t col_idx,
                                        const std::string &op, const Value &val) const {
    if (rg_idx >= meta_.row_groups.size()) return false;
    auto &rg = meta_.row_groups[rg_idx];
    if (col_idx >= rg.columns.size()) return true;
    auto &col = rg.columns[col_idx];
    if (!col.has_stats) return true;

    // Coerce the literal to the column's stat type so cross-type
    // comparisons don't silently bypass the zone-map check.
    Value v;
    if (!CoerceForCompare(val, col.min_value, v)) return true;

    // Use zone map logic.
    if (op == "=" || op == "==") {
        bool zone_might = !(v < col.min_value) && !(col.max_value < v);
        if (zone_might && DictSkipPossible(rg_idx, col_idx, op, v)) return false;
        return zone_might;
    } else if (op == ">") {
        return !(col.max_value < v) && !(col.max_value == v);
    } else if (op == ">=") {
        return !(col.max_value < v);
    } else if (op == "<") {
        return !(v < col.min_value) && !(col.min_value == v);
    } else if (op == "<=") {
        return !(v < col.min_value);
    } else if (op == "<>" || op == "!=") {
        // RG can be skipped only when every row equals v — i.e. min == v
        // AND max == v. ClickBench Q8/Q12 filter on AdvEngineID/MobilePhone
        // with `<> 0`, and many RGs are all-zero on these columns.
        return !(col.min_value == v && col.max_value == v);
    }
    return true;
}

} // namespace slothdb

#include "slothdb/storage/parquet.hpp"
#include "slothdb/common/exception.hpp"
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <string>

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
        case 5: { // min_value (preferred)
            uint64_t l = r.VarU64();
            m.min_bytes.assign(reinterpret_cast<const char*>(&r.data[r.pos]), l);
            r.pos += l; m.has_stats = true;
            break;
        }
        case 6: { // max_value (preferred)
            uint64_t l = r.VarU64();
            m.max_bytes.assign(reinterpret_cast<const char*>(&r.data[r.pos]), l);
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
};

// Decompress a page body (copy if uncompressed, Snappy expand otherwise).
bool DecompressPage(int32_t codec, const uint8_t *in, size_t compressed_size, size_t uncompressed_size,
                    std::vector<uint8_t> &out) {
    if (codec == 0) {
        out.assign(in, in + compressed_size);
        return out.size() == uncompressed_size;
    }
    if (codec == 1) {
        return SnappyDecompress(in, compressed_size, out);
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
    ReadMetadata();
}

void ParquetReader::ReadMetadata() {
    std::ifstream file(path_, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Parquet file: " + path_);

    auto file_size = file.tellg();
    if (file_size < 12) throw IOException(ErrorCode::CORRUPT_DATA, "File too small for Parquet");

    // Read magic at start.
    file.seekg(0);
    char magic_start[4];
    file.read(magic_start, 4);
    if (std::memcmp(magic_start, PARQUET_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not a Parquet file (bad magic)");

    // Read footer size and magic at end.
    file.seekg(static_cast<std::streamoff>(file_size) - 8);
    uint32_t footer_size;
    file.read(reinterpret_cast<char *>(&footer_size), 4);
    char magic_end[4];
    file.read(magic_end, 4);
    if (std::memcmp(magic_end, PARQUET_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not a Parquet file (bad end magic)");

    // Read footer.
    // Validate footer_size against file bounds.
    auto file_sz = static_cast<size_t>(file_size);
    if (footer_size > file_sz - 8)
        throw IOException(ErrorCode::CORRUPT_DATA, "Parquet footer size exceeds file");
    if (footer_size < 16)
        throw IOException(ErrorCode::CORRUPT_DATA, "Parquet footer too small");

    auto footer_offset = static_cast<std::streamoff>(file_sz - 8 - footer_size);
    std::vector<uint8_t> footer(footer_size);
    file.seekg(footer_offset);
    file.read(reinterpret_cast<char *>(footer.data()), footer_size);

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
                }
                meta_.row_groups.clear();
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

// Legacy (custom-format) column chunk reader.
static std::vector<Value> ReadColumnChunkLegacy(const std::string &path, const ParquetColumnMeta &meta) {
    std::ifstream file(path, std::ios::binary);
    file.seekg(meta.data_offset);

    std::vector<uint8_t> data(meta.data_size);
    file.read(reinterpret_cast<char *>(data.data()), meta.data_size);

    std::vector<Value> values;
    size_t pos = 0;

    for (int64_t i = 0; i < meta.num_values && pos < data.size(); i++) {
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
static size_t ReadOnePage(std::ifstream &file, int64_t offset, size_t file_remaining,
                         ParqPageHeader &hdr, std::vector<uint8_t> &raw_body) {
    // Read a chunk for the page header (headers are small, usually < 256 bytes
    // but allow up to a few KB to handle stats-embedded headers).
    file.seekg(offset);
    constexpr size_t HDR_READ = 4096;
    size_t to_read = std::min<size_t>(HDR_READ, file_remaining);
    std::vector<uint8_t> hdr_buf(to_read);
    file.read(reinterpret_cast<char*>(hdr_buf.data()), to_read);
    size_t consumed = ParsePageHeader(hdr_buf.data(), hdr_buf.size(), hdr);
    // Read compressed body.
    raw_body.resize(hdr.compressed_page_size);
    // Some of the body may already be in hdr_buf.
    size_t body_in_hdr = (to_read > consumed) ? std::min<size_t>(to_read - consumed, (size_t)hdr.compressed_page_size) : 0;
    if (body_in_hdr) std::memcpy(raw_body.data(), hdr_buf.data() + consumed, body_in_hdr);
    if ((size_t)hdr.compressed_page_size > body_in_hdr) {
        file.seekg(offset + static_cast<int64_t>(consumed + body_in_hdr));
        file.read(reinterpret_cast<char*>(raw_body.data() + body_in_hdr),
                  hdr.compressed_page_size - body_in_hdr);
    }
    return consumed + hdr.compressed_page_size;
}

// Decode a DataPage (V1 or V2) into `out` values. Handles PLAIN and PLAIN_DICTIONARY.
// For flat schemas with nullable columns, def_level bit_width is 1.
static void DecodeDataPage(const ParqPageHeader &hdr, const uint8_t *data, size_t size,
                           ParquetType ptype, const std::vector<Value> &dict,
                           std::vector<Value> &out) {
    const uint8_t *p = data;
    size_t remaining = size;

    // V1 pages start with RLE-packed def/rep levels prefixed by 4-byte little-endian length.
    // Flat, non-null schemas have no levels. Assume flat with possible nulls (max_def=1).
    std::vector<uint8_t> def_mask; // one byte per value (0 = null, 1 = present)
    int32_t n_values = hdr.num_values;

    if (hdr.type == 0) {
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

// Standard-Parquet column chunk reader.
static std::vector<Value> ReadColumnChunkStd(const std::string &path, const ParquetColumnMeta &meta) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Parquet: " + path);
    auto file_size = static_cast<size_t>(file.tellg());

    std::vector<Value> values;
    values.reserve(meta.num_values);

    // 1. Dictionary page (optional).
    std::vector<Value> dict;
    if (meta.dict_page_offset >= 0) {
        ParqPageHeader hdr;
        std::vector<uint8_t> raw;
        size_t remaining = file_size - static_cast<size_t>(meta.dict_page_offset);
        ReadOnePage(file, meta.dict_page_offset, remaining, hdr, raw);
        if (hdr.type == 2) {
            std::vector<uint8_t> decompressed;
            const uint8_t *body = raw.data();
            size_t body_size = raw.size();
            if (meta.codec != 0) {
                if (!DecompressPage(meta.codec, raw.data(), raw.size(),
                                     static_cast<size_t>(hdr.uncompressed_page_size), decompressed)) {
                    throw IOException(ErrorCode::CORRUPT_DATA,
                                      "Unsupported Parquet compression codec: " + std::to_string(meta.codec));
                }
                body = decompressed.data();
                body_size = decompressed.size();
            }
            DecodePlain(body, body_size, meta.parquet_type, hdr.dict_num_values, dict);
        }
    }

    // 2. Data pages (one or more, until num_values read).
    int64_t cur_offset = (meta.dict_page_offset >= 0)
        ? (meta.dict_page_offset + 0) // we'll find next offset via reading.
        : meta.data_offset;
    // Actually the data pages follow either after the dict page (end of
    // compressed body) or start at data_offset if no dict. The file layout
    // has the dict page body immediately after its header, then the data
    // page header at dict_end.
    if (meta.dict_page_offset >= 0) {
        // Recompute end of dict page via re-reading its header.
        ParqPageHeader hdr;
        std::vector<uint8_t> raw;
        size_t remaining = file_size - static_cast<size_t>(meta.dict_page_offset);
        size_t consumed = ReadOnePage(file, meta.dict_page_offset, remaining, hdr, raw);
        cur_offset = meta.dict_page_offset + static_cast<int64_t>(consumed);
    } else {
        cur_offset = meta.data_offset;
    }

    int64_t remaining_rows = meta.num_values;
    int64_t total_chunk_end = meta.data_offset + meta.total_compressed_size;
    // When the dict offset is before data_offset (common), data_offset points at first data page.
    if (meta.dict_page_offset >= 0 && cur_offset < meta.data_offset) {
        cur_offset = meta.data_offset;
    }

    while (remaining_rows > 0 && cur_offset < static_cast<int64_t>(file_size)) {
        ParqPageHeader hdr;
        std::vector<uint8_t> raw;
        size_t remaining_bytes = file_size - static_cast<size_t>(cur_offset);
        size_t consumed = ReadOnePage(file, cur_offset, remaining_bytes, hdr, raw);
        cur_offset += static_cast<int64_t>(consumed);

        if (hdr.type == 2) continue; // another dict page, skip

        const uint8_t *body = raw.data();
        size_t body_size = raw.size();
        std::vector<uint8_t> decompressed;
        bool compressed = (hdr.type == 3) ? hdr.is_compressed : (meta.codec != 0);
        if (compressed && meta.codec != 0) {
            if (!DecompressPage(meta.codec, raw.data(), raw.size(),
                                 static_cast<size_t>(hdr.uncompressed_page_size), decompressed)) {
                throw IOException(ErrorCode::CORRUPT_DATA,
                                  "Parquet decompression failed (codec=" + std::to_string(meta.codec) + ")");
            }
            body = decompressed.data();
            body_size = decompressed.size();
        }

        DecodeDataPage(hdr, body, body_size, meta.parquet_type, dict, values);
        remaining_rows -= hdr.num_values;

        if (cur_offset >= total_chunk_end) break;
    }

    return values;
}

std::vector<Value> ParquetReader::ReadColumnChunk(const ParquetColumnMeta &meta) {
    if (is_standard_parquet_) return ReadColumnChunkStd(path_, meta);
    return ReadColumnChunkLegacy(path_, meta);
}

std::vector<Value> ParquetReader::ReadColumn(idx_t rg_idx, idx_t col_idx) {
    if (rg_idx >= meta_.row_groups.size()) return {};
    auto &rg = meta_.row_groups[rg_idx];
    if (col_idx >= rg.columns.size()) return {};
    return ReadColumnChunk(rg.columns[col_idx]);
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

    // Write directly into DataChunk vectors — pack VECTOR_SIZE rows per chunk.
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

bool ParquetReader::RowGroupMightMatch(idx_t rg_idx, idx_t col_idx,
                                        const std::string &op, const Value &val) const {
    if (rg_idx >= meta_.row_groups.size()) return false;
    auto &rg = meta_.row_groups[rg_idx];
    if (col_idx >= rg.columns.size()) return true;
    auto &col = rg.columns[col_idx];
    if (!col.has_stats) return true;

    // Use zone map logic.
    if (op == "=" || op == "==") {
        return !(val < col.min_value) && !(col.max_value < val);
    } else if (op == ">") {
        return !(col.max_value < val) && !(col.max_value == val);
    } else if (op == ">=") {
        return !(col.max_value < val);
    } else if (op == "<") {
        return !(val < col.min_value) && !(col.min_value == val);
    } else if (op == "<=") {
        return !(val < col.min_value);
    }
    return true;
}

} // namespace slothdb

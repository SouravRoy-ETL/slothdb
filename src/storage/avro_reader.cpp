#include "slothdb/storage/avro_reader.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/types/string_type.hpp"
#include <fstream>
#include <cstring>

namespace slothdb {

static const uint8_t AVRO_MAGIC[4] = {'O', 'b', 'j', 0x01};

AvroReader::AvroReader(const std::string &path) : path_(path) {}

int64_t AvroReader::ReadVarInt(const uint8_t *data, size_t &pos, size_t size) {
    int64_t result = 0;
    int shift = 0;
    while (pos < size) {
        uint8_t b = data[pos++];
        result |= static_cast<int64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
    }
    // Zigzag decode.
    return (result >> 1) ^ -(result & 1);
}

std::string AvroReader::ReadAvroString(const uint8_t *data, size_t &pos, size_t size) {
    int64_t len = ReadVarInt(data, pos, size);
    if (len < 0) len = 0;
    auto slen = static_cast<size_t>(len);
    if (pos + slen > size) slen = size - pos;
    std::string result(reinterpret_cast<const char *>(&data[pos]), slen);
    pos += slen;
    return result;
}

void AvroReader::Parse() {
    if (parsed_) return;
    parsed_ = true;

    std::ifstream file(path_, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Avro file: " + path_);

    auto file_size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char *>(data.data()), file_size);

    // Check magic.
    if (file_size < 4 || std::memcmp(data.data(), AVRO_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not an Avro file");

    size_t pos = 4;

    // Read file header (metadata map).
    // Header is a map: [count(varint)][key(string), value(string)]...[0]
    std::string schema_json;
    std::string codec;
    uint8_t sync_marker[16] = {};

    while (pos < file_size) {
        int64_t count = ReadVarInt(data.data(), pos, file_size);
        if (count == 0) break;
        if (count < 0) {
            // Block size follows.
            ReadVarInt(data.data(), pos, file_size);
            count = -count;
        }
        for (int64_t i = 0; i < count; i++) {
            auto key = ReadAvroString(data.data(), pos, file_size);
            auto val = ReadAvroString(data.data(), pos, file_size);
            if (key == "avro.schema") schema_json = val;
            if (key == "avro.codec") codec = val;
        }
    }

    // Read sync marker (16 bytes).
    if (pos + 16 <= file_size) {
        std::memcpy(sync_marker, &data[pos], 16);
        pos += 16;
    }

    // Parse schema JSON to extract field names and types.
    // Schema format: {"type":"record","name":"...","fields":[{"name":"x","type":"int"}, ...]}
    // Simple JSON extraction:
    auto find_fields = schema_json.find("\"fields\"");
    if (find_fields != std::string::npos) {
        auto bracket = schema_json.find('[', find_fields);
        if (bracket != std::string::npos) {
            size_t sp = bracket + 1;
            while (sp < schema_json.size()) {
                auto name_pos = schema_json.find("\"name\"", sp);
                if (name_pos == std::string::npos) break;
                auto name_quote1 = schema_json.find('"', name_pos + 6);
                auto name_quote2 = schema_json.find('"', name_quote1 + 1);
                if (name_quote1 == std::string::npos) break;
                auto field_name = schema_json.substr(name_quote1 + 1,
                                                     name_quote2 - name_quote1 - 1);

                auto type_pos = schema_json.find("\"type\"", name_quote2);
                if (type_pos == std::string::npos) break;

                // Type can be a string or a union array.
                auto type_start = schema_json.find_first_of("\"[", type_pos + 6);
                std::string avro_type = "string";
                if (type_start != std::string::npos) {
                    if (schema_json[type_start] == '"') {
                        auto type_end = schema_json.find('"', type_start + 1);
                        avro_type = schema_json.substr(type_start + 1,
                                                        type_end - type_start - 1);
                    } else {
                        // Union: ["null", "int"] - pick the non-null type.
                        auto union_end = schema_json.find(']', type_start);
                        auto union_str = schema_json.substr(type_start, union_end - type_start + 1);
                        // Find non-null type.
                        for (auto &t : {"int", "long", "float", "double", "string", "boolean"}) {
                            if (union_str.find(t) != std::string::npos) {
                                avro_type = t;
                                break;
                            }
                        }
                    }
                }

                column_names_.push_back(field_name);
                avro_types_.push_back(avro_type);

                if (avro_type == "int") column_types_.push_back(LogicalType::INTEGER());
                else if (avro_type == "long") column_types_.push_back(LogicalType::BIGINT());
                else if (avro_type == "float") column_types_.push_back(LogicalType::FLOAT());
                else if (avro_type == "double") column_types_.push_back(LogicalType::DOUBLE());
                else if (avro_type == "boolean") column_types_.push_back(LogicalType::BOOLEAN());
                else column_types_.push_back(LogicalType::VARCHAR());

                sp = schema_json.find('}', type_pos) + 1;
                if (sp >= schema_json.size()) break;
            }
        }
    }

    // Read data blocks.
    while (pos + 16 < file_size) {
        int64_t object_count = ReadVarInt(data.data(), pos, file_size);
        int64_t block_size = ReadVarInt(data.data(), pos, file_size);
        if (object_count <= 0 || block_size <= 0) break;

        size_t block_end = pos + static_cast<size_t>(block_size);
        if (block_end > file_size) break;

        for (int64_t obj = 0; obj < object_count && pos < block_end; obj++) {
            std::vector<Value> row;
            for (size_t c = 0; c < column_types_.size(); c++) {
                auto &avro_type = avro_types_[c];

                // Handle nullable (union with null).
                // In Avro binary, unions have an index prefix.
                // For ["null", "type"], index 0 = null, index 1 = value.
                // Skip this complexity for simple types.

                if (avro_type == "int") {
                    int32_t val = static_cast<int32_t>(ReadVarInt(data.data(), pos, block_end));
                    row.push_back(Value::INTEGER(val));
                } else if (avro_type == "long") {
                    int64_t val = ReadVarInt(data.data(), pos, block_end);
                    row.push_back(Value::BIGINT(val));
                } else if (avro_type == "float") {
                    float val;
                    if (pos + 4 <= block_end) {
                        std::memcpy(&val, &data[pos], 4); pos += 4;
                        row.push_back(Value::FLOAT(val));
                    } else row.push_back(Value());
                } else if (avro_type == "double") {
                    double val;
                    if (pos + 8 <= block_end) {
                        std::memcpy(&val, &data[pos], 8); pos += 8;
                        row.push_back(Value::DOUBLE(val));
                    } else row.push_back(Value());
                } else if (avro_type == "boolean") {
                    if (pos < block_end) {
                        row.push_back(Value::BOOLEAN(data[pos++] != 0));
                    } else row.push_back(Value());
                } else {
                    // String/bytes.
                    auto str = ReadAvroString(data.data(), pos, block_end);
                    row.push_back(Value::VARCHAR(str));
                }
            }
            rows_.push_back(std::move(row));
        }

        // Skip sync marker.
        pos = block_end;
        if (pos + 16 <= file_size) pos += 16;
    }
}

std::vector<std::vector<Value>> AvroReader::ReadAll() {
    Parse();
    return rows_;
}

// ============================================================================
// Lightweight schema detection - reads only the Avro header (magic + metadata
// map + sync marker) to populate `column_names_` / `column_types_`, then
// stops. Used by PhysicalAvroScan so catalog setup doesn't trigger a full
// data-block parse.
// ============================================================================
void AvroReader::DetectSchemaLight() {
    if (!column_names_.empty()) return; // already populated

    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Avro file: " + path_);

    // Avro headers are small - 64 KB is plenty for the metadata map.
    const size_t HEAD = 64 * 1024;
    std::vector<uint8_t> data(HEAD);
    file.read(reinterpret_cast<char *>(data.data()), HEAD);
    size_t file_size = (size_t)file.gcount();
    if (file_size < 4 || std::memcmp(data.data(), AVRO_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not an Avro file");

    size_t pos = 4;
    std::string schema_json;
    while (pos < file_size) {
        int64_t count = ReadVarInt(data.data(), pos, file_size);
        if (count == 0) break;
        if (count < 0) { ReadVarInt(data.data(), pos, file_size); count = -count; }
        for (int64_t i = 0; i < count; i++) {
            auto key = ReadAvroString(data.data(), pos, file_size);
            auto val = ReadAvroString(data.data(), pos, file_size);
            if (key == "avro.schema") schema_json = val;
        }
    }

    // Parse schema JSON to extract field names and types - same tiny extractor
    // as Parse() but bailing before the data-block loop.
    auto find_fields = schema_json.find("\"fields\"");
    if (find_fields != std::string::npos) {
        auto bracket = schema_json.find('[', find_fields);
        if (bracket != std::string::npos) {
            size_t sp = bracket + 1;
            while (sp < schema_json.size()) {
                auto name_pos = schema_json.find("\"name\"", sp);
                if (name_pos == std::string::npos) break;
                auto nq1 = schema_json.find('"', name_pos + 6);
                auto nq2 = schema_json.find('"', nq1 + 1);
                if (nq1 == std::string::npos) break;
                auto field_name = schema_json.substr(nq1 + 1, nq2 - nq1 - 1);
                auto type_pos = schema_json.find("\"type\"", nq2);
                if (type_pos == std::string::npos) break;
                auto type_start = schema_json.find_first_of("\"[", type_pos + 6);
                std::string avro_type = "string";
                if (type_start != std::string::npos) {
                    if (schema_json[type_start] == '"') {
                        auto type_end = schema_json.find('"', type_start + 1);
                        avro_type = schema_json.substr(type_start + 1, type_end - type_start - 1);
                    } else {
                        auto uend = schema_json.find(']', type_start);
                        auto us = schema_json.substr(type_start, uend - type_start + 1);
                        for (auto &t : {"int","long","float","double","string","boolean"}) {
                            if (us.find(t) != std::string::npos) { avro_type = t; break; }
                        }
                    }
                }
                column_names_.push_back(field_name);
                avro_types_.push_back(avro_type);
                if (avro_type == "int") column_types_.push_back(LogicalType::INTEGER());
                else if (avro_type == "long") column_types_.push_back(LogicalType::BIGINT());
                else if (avro_type == "float") column_types_.push_back(LogicalType::FLOAT());
                else if (avro_type == "double") column_types_.push_back(LogicalType::DOUBLE());
                else if (avro_type == "boolean") column_types_.push_back(LogicalType::BOOLEAN());
                else column_types_.push_back(LogicalType::VARCHAR());
                sp = schema_json.find('}', type_pos) + 1;
                if (sp >= schema_json.size()) break;
            }
        }
    }
}

// ============================================================================
// Stream parse directly into typed DataChunks - no per-cell Value boxing,
// no rows_ vector. Each block decode writes straight into the current
// chunk's Vector buffers; when the chunk hits VECTOR_SIZE it's moved into
// `chunks` and a fresh one begins.
// ============================================================================
void AvroReader::ReadIntoChunks(std::vector<DataChunk> &chunks,
                                 const std::vector<LogicalType> &types) {
    if (column_names_.empty()) DetectSchemaLight();

    std::ifstream file(path_, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Avro file: " + path_);
    auto file_size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char *>(data.data()), file_size);

    if (file_size < 4 || std::memcmp(data.data(), AVRO_MAGIC, 4) != 0)
        throw IOException(ErrorCode::CORRUPT_DATA, "Not an Avro file");

    size_t pos = 4;
    // Re-scan the header map to find end-of-header (we don't need the schema
    // JSON again - already have it via DetectSchemaLight).
    while (pos < file_size) {
        int64_t count = ReadVarInt(data.data(), pos, file_size);
        if (count == 0) break;
        if (count < 0) { ReadVarInt(data.data(), pos, file_size); count = -count; }
        for (int64_t i = 0; i < count; i++) {
            ReadAvroString(data.data(), pos, file_size);
            ReadAvroString(data.data(), pos, file_size);
        }
    }
    if (pos + 16 <= file_size) pos += 16; // sync marker

    const idx_t ncols = column_types_.size();
    if (ncols == 0) return;

    // Per-column pointer cache for the active chunk.
    struct ColPtr {
        LogicalTypeId tid;
        data_ptr_t data;
        ValidityMask *validity;
        VectorStringBuffer *str_buf;
    };
    std::vector<ColPtr> cp(ncols);
    DataChunk chunk;
    chunk.Initialize(types);
    auto rebind = [&]() {
        for (idx_t c = 0; c < ncols; c++) {
            auto &vec = chunk.GetVector(c);
            cp[c].tid = types[c].id();
            cp[c].data = vec.GetData();
            cp[c].validity = &vec.GetValidity();
            cp[c].str_buf = (cp[c].tid == LogicalTypeId::VARCHAR)
                                ? &vec.GetStringBuffer() : nullptr;
        }
    };
    rebind();
    idx_t count = 0;

    while (pos + 16 < file_size) {
        int64_t object_count = ReadVarInt(data.data(), pos, file_size);
        int64_t block_size = ReadVarInt(data.data(), pos, file_size);
        if (object_count <= 0 || block_size <= 0) break;
        size_t block_end = pos + static_cast<size_t>(block_size);
        if (block_end > file_size) break;

        for (int64_t obj = 0; obj < object_count && pos < block_end; obj++) {
            for (size_t c = 0; c < ncols; c++) {
                const auto &avro_type = avro_types_[c];
                auto tid = cp[c].tid;
                if (avro_type == "int") {
                    int32_t v = static_cast<int32_t>(
                        ReadVarInt(data.data(), pos, block_end));
                    if (tid == LogicalTypeId::INTEGER)
                        reinterpret_cast<int32_t *>(cp[c].data)[count] = v;
                    else if (tid == LogicalTypeId::BIGINT)
                        reinterpret_cast<int64_t *>(cp[c].data)[count] = (int64_t)v;
                } else if (avro_type == "long") {
                    int64_t v = ReadVarInt(data.data(), pos, block_end);
                    if (tid == LogicalTypeId::BIGINT)
                        reinterpret_cast<int64_t *>(cp[c].data)[count] = v;
                    else if (tid == LogicalTypeId::INTEGER)
                        reinterpret_cast<int32_t *>(cp[c].data)[count] = (int32_t)v;
                } else if (avro_type == "float") {
                    float v = 0;
                    if (pos + 4 <= block_end) {
                        std::memcpy(&v, &data[pos], 4); pos += 4;
                    }
                    reinterpret_cast<float *>(cp[c].data)[count] = v;
                } else if (avro_type == "double") {
                    double v = 0;
                    if (pos + 8 <= block_end) {
                        std::memcpy(&v, &data[pos], 8); pos += 8;
                    }
                    reinterpret_cast<double *>(cp[c].data)[count] = v;
                } else if (avro_type == "boolean") {
                    uint8_t v = (pos < block_end) ? data[pos++] : 0;
                    reinterpret_cast<bool *>(cp[c].data)[count] = v != 0;
                } else {
                    // String: length-prefixed bytes.
                    int64_t slen = ReadVarInt(data.data(), pos, block_end);
                    if (slen < 0) slen = 0;
                    size_t n = (size_t)slen;
                    if (pos + n > block_end) n = block_end - pos;
                    const char *src = reinterpret_cast<const char *>(&data[pos]);
                    pos += n;
                    auto *arr = reinterpret_cast<string_t *>(cp[c].data);
                    if (n <= string_t::INLINE_LENGTH) {
                        arr[count] = string_t(src, (uint32_t)n);
                    } else {
                        const char *heap = cp[c].str_buf->AddString(src, n);
                        arr[count] = string_t(heap, (uint32_t)n);
                    }
                }
            }
            count++;
            if (count == VECTOR_SIZE) {
                chunk.SetCardinality(count);
                chunks.push_back(std::move(chunk));
                chunk = DataChunk{};
                chunk.Initialize(types);
                rebind();
                count = 0;
            }
        }

        pos = block_end;
        if (pos + 16 <= file_size) pos += 16; // sync marker
    }

    if (count > 0) {
        chunk.SetCardinality(count);
        chunks.push_back(std::move(chunk));
    }
}

} // namespace slothdb

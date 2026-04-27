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

// Map an Avro primitive type name + optional logicalType to a SlothDB
// LogicalType. Logical types we recognise:
//   - timestamp-millis / timestamp-micros → TIMESTAMP (microseconds)
//   - date                                → DATE (days)
//   - time-millis / time-micros           → TIME (microseconds)
// Anything else falls through to the raw avro primitive mapping.
static LogicalType AvroTypeToLogical(const std::string &avro_type,
                                      const std::string &logical_type,
                                      int64_t &out_ts_scale) {
    out_ts_scale = 0;
    if (logical_type == "timestamp-millis") {
        out_ts_scale = 1000;  // ms -> us
        return LogicalType::TIMESTAMP();
    }
    if (logical_type == "timestamp-micros") {
        out_ts_scale = 1;
        return LogicalType::TIMESTAMP();
    }
    if (logical_type == "date") {
        return LogicalType::DATE();
    }
    if (logical_type == "time-micros" || logical_type == "time-millis") {
        out_ts_scale = (logical_type == "time-millis") ? 1000 : 1;
        return LogicalType::TIME();
    }
    if (avro_type == "int") return LogicalType::INTEGER();
    if (avro_type == "long") return LogicalType::BIGINT();
    if (avro_type == "float") return LogicalType::FLOAT();
    if (avro_type == "double") return LogicalType::DOUBLE();
    if (avro_type == "boolean") return LogicalType::BOOLEAN();
    return LogicalType::VARCHAR();
}

// Parse a single field's "type" fragment and produce:
//   - the avro primitive name we'll use at wire-decode time
//   - whether the field is nullable (["null", T] union)
//   - the SlothDB LogicalType (incl. logical-type-driven DATE/TIMESTAMP)
//   - a scale factor for timestamp-millis -> microsecond conversion
// `field_schema` is the substring of the JSON schema starting at the
// field's "type" key, ending at the closing brace of the field record.
void AvroReader::ParseFieldSchema(const std::string &field_schema,
                                   std::string &out_avro_type,
                                   bool &out_nullable,
                                   int64_t &out_ts_scale,
                                   LogicalType &out_logical_type) {
    out_avro_type = "string";
    out_nullable = false;
    out_ts_scale = 0;
    std::string logical_type_name;

    auto type_pos = field_schema.find("\"type\"");
    if (type_pos == std::string::npos) {
        out_logical_type = LogicalType::VARCHAR();
        return;
    }

    // The character after "type": is either '"' (primitive), '[' (union),
    // or '{' (record/logical-type wrapper). Scan from after "type".
    auto type_start = field_schema.find_first_of("\"[{", type_pos + 6);
    if (type_start == std::string::npos) {
        out_logical_type = LogicalType::VARCHAR();
        return;
    }

    char first = field_schema[type_start];
    if (first == '[') {
        // Union. Detect ["null", T] (nullable) — extract T's primitive name.
        auto union_end = field_schema.find(']', type_start);
        std::string union_str = (union_end == std::string::npos)
            ? field_schema.substr(type_start)
            : field_schema.substr(type_start, union_end - type_start + 1);
        if (union_str.find("\"null\"") != std::string::npos) {
            out_nullable = true;
        }
        for (auto &t : {"int", "long", "float", "double", "string", "boolean"}) {
            if (union_str.find(std::string("\"") + t + "\"") != std::string::npos) {
                out_avro_type = t;
                break;
            }
            // Logical-type wrapper inside union: {"type":"long","logicalType":"timestamp-millis"}
            if (union_str.find(std::string(":\"") + t + "\"") != std::string::npos) {
                out_avro_type = t;
            }
        }
        // Pick up logicalType from the wrapper, if any.
        auto lt_pos = union_str.find("\"logicalType\"");
        if (lt_pos != std::string::npos) {
            auto q1 = union_str.find('"', lt_pos + 13);
            auto q2 = (q1 == std::string::npos) ? std::string::npos
                                                 : union_str.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                logical_type_name = union_str.substr(q1 + 1, q2 - q1 - 1);
        }
    } else if (first == '"') {
        // Primitive type.
        auto type_end = field_schema.find('"', type_start + 1);
        if (type_end != std::string::npos)
            out_avro_type = field_schema.substr(type_start + 1, type_end - type_start - 1);
    } else if (first == '{') {
        // Logical-type wrapper, e.g. {"type":"long","logicalType":"timestamp-millis"}.
        // The inner record has its own "type" and "logicalType" keys; locate
        // the *first* such pair after the brace and read them.
        auto inner_close = field_schema.find('}', type_start);
        std::string inner = (inner_close == std::string::npos)
            ? field_schema.substr(type_start)
            : field_schema.substr(type_start, inner_close - type_start + 1);
        for (auto &t : {"int", "long", "float", "double", "string", "boolean"}) {
            if (inner.find(std::string(":\"") + t + "\"") != std::string::npos) {
                out_avro_type = t;
                break;
            }
        }
        auto lt_pos = inner.find("\"logicalType\"");
        if (lt_pos != std::string::npos) {
            auto q1 = inner.find('"', lt_pos + 13);
            auto q2 = (q1 == std::string::npos) ? std::string::npos
                                                 : inner.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                logical_type_name = inner.substr(q1 + 1, q2 - q1 - 1);
        }
    }

    out_logical_type = AvroTypeToLogical(out_avro_type, logical_type_name, out_ts_scale);
}

// Slice the schema JSON into one substring per field. Each substring
// covers from the field's "name" key to the matching closing brace
// of the field record. Brace-balanced, so logical-type wrappers don't
// break us. Used by both Parse() and DetectSchemaLight() to drive
// ParseFieldSchema, which extracts the per-field metadata we need.
static std::vector<std::pair<std::string, std::string>>
SplitSchemaFields(const std::string &schema_json) {
    std::vector<std::pair<std::string, std::string>> out;
    auto find_fields = schema_json.find("\"fields\"");
    if (find_fields == std::string::npos) return out;
    auto bracket = schema_json.find('[', find_fields);
    if (bracket == std::string::npos) return out;

    size_t sp = bracket + 1;
    while (sp < schema_json.size()) {
        // Skip whitespace and commas.
        while (sp < schema_json.size() &&
               (schema_json[sp] == ' ' || schema_json[sp] == ',' ||
                schema_json[sp] == '\n' || schema_json[sp] == '\r' ||
                schema_json[sp] == '\t'))
            sp++;
        if (sp >= schema_json.size() || schema_json[sp] == ']') break;
        if (schema_json[sp] != '{') break;

        // Brace-balanced scan for the matching close.
        int depth = 0;
        size_t start = sp;
        bool in_str = false;
        for (; sp < schema_json.size(); sp++) {
            char ch = schema_json[sp];
            if (in_str) {
                if (ch == '\\' && sp + 1 < schema_json.size()) { sp++; continue; }
                if (ch == '"') in_str = false;
                continue;
            }
            if (ch == '"') in_str = true;
            else if (ch == '{') depth++;
            else if (ch == '}') {
                depth--;
                if (depth == 0) { sp++; break; }
            }
        }
        std::string field_str = schema_json.substr(start, sp - start);

        // Pull out the field name. Layout: `"name":"X"` — first quote after
        // the "name" key opens the value string, second quote closes it.
        std::string field_name;
        auto name_pos = field_str.find("\"name\"");
        if (name_pos != std::string::npos) {
            auto q1 = field_str.find('"', name_pos + 6);
            auto q2 = (q1 == std::string::npos) ? std::string::npos
                                                 : field_str.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                field_name = field_str.substr(q1 + 1, q2 - q1 - 1);
        }
        out.emplace_back(field_name, field_str);
    }
    return out;
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

    if (pos + 16 <= file_size) {
        std::memcpy(sync_marker, &data[pos], 16);
        pos += 16;
    }

    // Slice schema into fields and extract per-column metadata.
    auto fields = SplitSchemaFields(schema_json);
    for (auto &f : fields) {
        std::string avro_type;
        bool nullable = false;
        int64_t ts_scale = 0;
        LogicalType lt = LogicalType::VARCHAR();
        ParseFieldSchema(f.second, avro_type, nullable, ts_scale, lt);
        column_names_.push_back(f.first);
        avro_types_.push_back(avro_type);
        avro_nullable_.push_back(nullable ? 1 : 0);
        avro_ts_scale_.push_back(ts_scale);
        column_types_.push_back(lt);
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
                bool is_null = false;
                if (avro_nullable_[c]) {
                    int64_t idx = ReadVarInt(data.data(), pos, block_end);
                    // Avro union convention: index 0 = "null" branch when
                    // the schema is ["null", T]; non-zero = the value branch.
                    is_null = (idx == 0);
                }
                if (is_null) {
                    row.push_back(Value());
                    continue;
                }
                if (avro_type == "int") {
                    int32_t val = static_cast<int32_t>(ReadVarInt(data.data(), pos, block_end));
                    row.push_back(Value::INTEGER(val));
                } else if (avro_type == "long") {
                    int64_t val = ReadVarInt(data.data(), pos, block_end);
                    row.push_back(Value::BIGINT(val));
                } else if (avro_type == "float") {
                    float val = 0;
                    if (pos + 4 <= block_end) {
                        std::memcpy(&val, &data[pos], 4); pos += 4;
                    }
                    row.push_back(Value::FLOAT(val));
                } else if (avro_type == "double") {
                    double val = 0;
                    if (pos + 8 <= block_end) {
                        std::memcpy(&val, &data[pos], 8); pos += 8;
                    }
                    row.push_back(Value::DOUBLE(val));
                } else if (avro_type == "boolean") {
                    if (pos < block_end) {
                        row.push_back(Value::BOOLEAN(data[pos++] != 0));
                    } else row.push_back(Value());
                } else {
                    auto str = ReadAvroString(data.data(), pos, block_end);
                    row.push_back(Value::VARCHAR(str));
                }
            }
            rows_.push_back(std::move(row));
        }

        pos = block_end;
        if (pos + 16 <= file_size) pos += 16;
    }
}

std::vector<std::vector<Value>> AvroReader::ReadAll() {
    Parse();
    return rows_;
}

void AvroReader::DetectSchemaLight() {
    if (!column_names_.empty()) return;

    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open())
        throw IOException(ErrorCode::FILE_NOT_FOUND, "Cannot open Avro file: " + path_);

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

    auto fields = SplitSchemaFields(schema_json);
    for (auto &f : fields) {
        std::string avro_type;
        bool nullable = false;
        int64_t ts_scale = 0;
        LogicalType lt = LogicalType::VARCHAR();
        ParseFieldSchema(f.second, avro_type, nullable, ts_scale, lt);
        column_names_.push_back(f.first);
        avro_types_.push_back(avro_type);
        avro_nullable_.push_back(nullable ? 1 : 0);
        avro_ts_scale_.push_back(ts_scale);
        column_types_.push_back(lt);
    }
}

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
    while (pos < file_size) {
        int64_t count = ReadVarInt(data.data(), pos, file_size);
        if (count == 0) break;
        if (count < 0) { ReadVarInt(data.data(), pos, file_size); count = -count; }
        for (int64_t i = 0; i < count; i++) {
            ReadAvroString(data.data(), pos, file_size);
            ReadAvroString(data.data(), pos, file_size);
        }
    }
    if (pos + 16 <= file_size) pos += 16;

    const idx_t ncols = column_types_.size();
    if (ncols == 0) return;

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
                int64_t ts_scale = avro_ts_scale_[c];

                // Nullable field: read and check the union index byte.
                bool is_null = false;
                if (avro_nullable_[c]) {
                    int64_t idx = ReadVarInt(data.data(), pos, block_end);
                    is_null = (idx == 0);
                }
                if (is_null) {
                    cp[c].validity->SetInvalid(count);
                    // Zero-fill the slot so downstream code that ignores the
                    // validity mask doesn't read uninitialised memory.
                    if (tid == LogicalTypeId::INTEGER || tid == LogicalTypeId::DATE)
                        reinterpret_cast<int32_t *>(cp[c].data)[count] = 0;
                    else if (tid == LogicalTypeId::BIGINT ||
                             tid == LogicalTypeId::TIMESTAMP ||
                             tid == LogicalTypeId::TIMESTAMP_TZ ||
                             tid == LogicalTypeId::TIME)
                        reinterpret_cast<int64_t *>(cp[c].data)[count] = 0;
                    else if (tid == LogicalTypeId::FLOAT)
                        reinterpret_cast<float *>(cp[c].data)[count] = 0.0f;
                    else if (tid == LogicalTypeId::DOUBLE)
                        reinterpret_cast<double *>(cp[c].data)[count] = 0.0;
                    else if (tid == LogicalTypeId::BOOLEAN)
                        reinterpret_cast<bool *>(cp[c].data)[count] = false;
                    else if (tid == LogicalTypeId::VARCHAR)
                        reinterpret_cast<string_t *>(cp[c].data)[count] = string_t("", 0);
                    continue;
                }

                if (avro_type == "int") {
                    int32_t v = static_cast<int32_t>(
                        ReadVarInt(data.data(), pos, block_end));
                    if (tid == LogicalTypeId::INTEGER || tid == LogicalTypeId::DATE)
                        reinterpret_cast<int32_t *>(cp[c].data)[count] = v;
                    else if (tid == LogicalTypeId::BIGINT ||
                             tid == LogicalTypeId::TIMESTAMP ||
                             tid == LogicalTypeId::TIME)
                        reinterpret_cast<int64_t *>(cp[c].data)[count] = (int64_t)v;
                } else if (avro_type == "long") {
                    int64_t v = ReadVarInt(data.data(), pos, block_end);
                    // timestamp-millis comes in as ms; SlothDB TIMESTAMP is
                    // microseconds. Convert in-place.
                    if (ts_scale == 1000) v *= 1000;
                    if (tid == LogicalTypeId::BIGINT ||
                        tid == LogicalTypeId::TIMESTAMP ||
                        tid == LogicalTypeId::TIMESTAMP_TZ ||
                        tid == LogicalTypeId::TIME)
                        reinterpret_cast<int64_t *>(cp[c].data)[count] = v;
                    else if (tid == LogicalTypeId::INTEGER ||
                             tid == LogicalTypeId::DATE)
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
        if (pos + 16 <= file_size) pos += 16;
    }

    if (count > 0) {
        chunk.SetCardinality(count);
        chunks.push_back(std::move(chunk));
    }
}

} // namespace slothdb

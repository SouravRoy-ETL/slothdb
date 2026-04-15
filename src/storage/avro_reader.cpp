#include "slothdb/storage/avro_reader.hpp"
#include "slothdb/common/exception.hpp"
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
                        // Union: ["null", "int"] — pick the non-null type.
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

} // namespace slothdb

#include "slothdb/storage/compression/compression.hpp"
#include "slothdb/common/exception.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <limits>

namespace slothdb {

const char *CompressionTypeToString(CompressionType type) {
    switch (type) {
    case CompressionType::UNCOMPRESSED: return "UNCOMPRESSED";
    case CompressionType::CONSTANT: return "CONSTANT";
    case CompressionType::RLE: return "RLE";
    case CompressionType::DICTIONARY: return "DICTIONARY";
    case CompressionType::BITPACKING: return "BITPACKING";
    }
    return "UNKNOWN";
}

// ============================================================================
// Zone Map
// ============================================================================

bool ZoneMap::MightContain(const std::string &op, const Value &val) const {
    if (!has_stats || val.IsNull()) return true; // Can't skip.

    if (op == "=" || op == "==") {
        return !(val < min_value) && !(max_value < val);
    } else if (op == ">") {
        return !(max_value < val) && max_value != val;
    } else if (op == ">=") {
        return !(max_value < val);
    } else if (op == "<") {
        return !(val < min_value) && min_value != val;
    } else if (op == "<=") {
        return !(val < min_value);
    }
    return true; // Can't determine, don't skip.
}

ZoneMap CompressionManager::ComputeZoneMap(const std::vector<Value> &values) {
    ZoneMap zm;
    zm.count = static_cast<idx_t>(values.size());
    for (auto &v : values) {
        if (v.IsNull()) {
            zm.null_count++;
            continue;
        }
        if (!zm.has_stats) {
            zm.min_value = v;
            zm.max_value = v;
            zm.has_stats = true;
        } else {
            if (v < zm.min_value) zm.min_value = v;
            if (zm.max_value < v) zm.max_value = v;
        }
    }
    return zm;
}

// ============================================================================
// Uncompressed Codec
// ============================================================================

class UncompressedCodec : public CompressionCodec {
public:
    CompressedBuffer Compress(const std::vector<Value> &values,
                               const LogicalType &type) override {
        CompressedBuffer buf;
        buf.type = CompressionType::UNCOMPRESSED;
        buf.count = static_cast<idx_t>(values.size());

        // Serialize values to bytes: [null_flags][data]
        // For each value: 1 byte null flag + serialized data.
        for (auto &v : values) {
            buf.data.push_back(v.IsNull() ? 1 : 0);
            if (!v.IsNull()) {
                auto s = v.ToString();
                uint32_t len = static_cast<uint32_t>(s.size());
                auto *len_ptr = reinterpret_cast<uint8_t *>(&len);
                buf.data.insert(buf.data.end(), len_ptr, len_ptr + 4);
                buf.data.insert(buf.data.end(), s.begin(), s.end());
            }
        }
        return buf;
    }

    std::vector<Value> Decompress(const CompressedBuffer &buffer,
                                    const LogicalType &type) override {
        std::vector<Value> result;
        size_t pos = 0;
        auto &data = buffer.data;

        for (idx_t i = 0; i < buffer.count; i++) {
            bool is_null = data[pos++] != 0;
            if (is_null) {
                result.push_back(Value());
            } else {
                uint32_t len;
                std::memcpy(&len, &data[pos], 4);
                pos += 4;
                std::string s(reinterpret_cast<const char *>(&data[pos]), len);
                pos += len;
                result.push_back(StringToValue(s, type));
            }
        }
        return result;
    }

    CompressionType GetType() const override { return CompressionType::UNCOMPRESSED; }

private:
    static Value StringToValue(const std::string &s, const LogicalType &type) {
        switch (type.id()) {
        case LogicalTypeId::INTEGER: return Value::INTEGER(std::stoi(s));
        case LogicalTypeId::BIGINT: return Value::BIGINT(std::stoll(s));
        case LogicalTypeId::DOUBLE: return Value::DOUBLE(std::stod(s));
        case LogicalTypeId::FLOAT: return Value::FLOAT(std::stof(s));
        case LogicalTypeId::BOOLEAN: return Value::BOOLEAN(s == "true" || s == "1");
        case LogicalTypeId::VARCHAR: return Value::VARCHAR(s);
        default: return Value::VARCHAR(s);
        }
    }
};

// ============================================================================
// Constant Codec — all values are the same.
// ============================================================================

class ConstantCodec : public CompressionCodec {
public:
    CompressedBuffer Compress(const std::vector<Value> &values,
                               const LogicalType &type) override {
        CompressedBuffer buf;
        buf.type = CompressionType::CONSTANT;
        buf.count = static_cast<idx_t>(values.size());

        // Store just the single value + null bitmap.
        // First: the constant value as string.
        Value cv;
        bool all_null = true;
        for (auto &v : values) {
            if (!v.IsNull()) { cv = v; all_null = false; break; }
        }

        buf.data.push_back(all_null ? 1 : 0);
        if (!all_null) {
            auto s = cv.ToString();
            uint32_t len = static_cast<uint32_t>(s.size());
            auto *lp = reinterpret_cast<uint8_t *>(&len);
            buf.data.insert(buf.data.end(), lp, lp + 4);
            buf.data.insert(buf.data.end(), s.begin(), s.end());
        }

        // Null bitmap.
        for (auto &v : values) {
            buf.data.push_back(v.IsNull() ? 1 : 0);
        }
        return buf;
    }

    std::vector<Value> Decompress(const CompressedBuffer &buffer,
                                    const LogicalType &type) override {
        size_t pos = 0;
        auto &data = buffer.data;
        bool all_null = data[pos++] != 0;

        Value cv;
        if (!all_null) {
            uint32_t len;
            std::memcpy(&len, &data[pos], 4);
            pos += 4;
            std::string s(reinterpret_cast<const char *>(&data[pos]), len);
            pos += len;
            cv = UncompressedCodec().Decompress(
                CompressedBuffer{CompressionType::UNCOMPRESSED, {}, 0, {}}, type).empty()
                ? Value::VARCHAR(s) : Value::VARCHAR(s);
            // Simple: reconstruct from string.
            switch (type.id()) {
            case LogicalTypeId::INTEGER: cv = Value::INTEGER(std::stoi(s)); break;
            case LogicalTypeId::BIGINT: cv = Value::BIGINT(std::stoll(s)); break;
            case LogicalTypeId::DOUBLE: cv = Value::DOUBLE(std::stod(s)); break;
            case LogicalTypeId::VARCHAR: cv = Value::VARCHAR(s); break;
            default: cv = Value::VARCHAR(s); break;
            }
        }

        std::vector<Value> result;
        for (idx_t i = 0; i < buffer.count; i++) {
            bool is_null = data[pos++] != 0;
            result.push_back(is_null ? Value() : cv);
        }
        return result;
    }

    CompressionType GetType() const override { return CompressionType::CONSTANT; }
};

// ============================================================================
// RLE Codec — run-length encoding.
// ============================================================================

class RLECodec : public CompressionCodec {
public:
    CompressedBuffer Compress(const std::vector<Value> &values,
                               const LogicalType &type) override {
        CompressedBuffer buf;
        buf.type = CompressionType::RLE;
        buf.count = static_cast<idx_t>(values.size());

        // Format: [num_runs][run_length, value_string]...
        std::vector<std::pair<uint32_t, std::string>> runs;
        for (size_t i = 0; i < values.size();) {
            auto &v = values[i];
            auto s = v.IsNull() ? "__NULL__" : v.ToString();
            uint32_t run_len = 1;
            while (i + run_len < values.size()) {
                auto &next = values[i + run_len];
                auto ns = next.IsNull() ? "__NULL__" : next.ToString();
                if (ns != s) break;
                run_len++;
            }
            runs.push_back({run_len, s});
            i += run_len;
        }

        uint32_t num_runs = static_cast<uint32_t>(runs.size());
        auto *rp = reinterpret_cast<uint8_t *>(&num_runs);
        buf.data.insert(buf.data.end(), rp, rp + 4);

        for (auto &[len, val] : runs) {
            auto *lp = reinterpret_cast<uint8_t *>(&len);
            buf.data.insert(buf.data.end(), lp, lp + 4);
            uint32_t slen = static_cast<uint32_t>(val.size());
            auto *sp = reinterpret_cast<uint8_t *>(&slen);
            buf.data.insert(buf.data.end(), sp, sp + 4);
            buf.data.insert(buf.data.end(), val.begin(), val.end());
        }
        return buf;
    }

    std::vector<Value> Decompress(const CompressedBuffer &buffer,
                                    const LogicalType &type) override {
        std::vector<Value> result;
        size_t pos = 0;
        auto &data = buffer.data;

        uint32_t num_runs;
        std::memcpy(&num_runs, &data[pos], 4); pos += 4;

        for (uint32_t r = 0; r < num_runs; r++) {
            uint32_t run_len;
            std::memcpy(&run_len, &data[pos], 4); pos += 4;
            uint32_t slen;
            std::memcpy(&slen, &data[pos], 4); pos += 4;
            std::string val(reinterpret_cast<const char *>(&data[pos]), slen);
            pos += slen;

            Value v;
            if (val == "__NULL__") {
                v = Value();
            } else {
                switch (type.id()) {
                case LogicalTypeId::INTEGER: v = Value::INTEGER(std::stoi(val)); break;
                case LogicalTypeId::BIGINT: v = Value::BIGINT(std::stoll(val)); break;
                case LogicalTypeId::DOUBLE: v = Value::DOUBLE(std::stod(val)); break;
                case LogicalTypeId::VARCHAR: v = Value::VARCHAR(val); break;
                default: v = Value::VARCHAR(val); break;
                }
            }

            for (uint32_t i = 0; i < run_len; i++) {
                result.push_back(v);
            }
        }
        return result;
    }

    CompressionType GetType() const override { return CompressionType::RLE; }
};

// ============================================================================
// Dictionary Codec — for low-cardinality columns.
// ============================================================================

class DictionaryCodec : public CompressionCodec {
public:
    CompressedBuffer Compress(const std::vector<Value> &values,
                               const LogicalType &type) override {
        CompressedBuffer buf;
        buf.type = CompressionType::DICTIONARY;
        buf.count = static_cast<idx_t>(values.size());

        // Build dictionary: unique values -> index.
        std::unordered_map<std::string, uint32_t> dict;
        std::vector<std::string> dict_entries;
        std::vector<uint32_t> indices;
        std::vector<bool> nulls;

        for (auto &v : values) {
            if (v.IsNull()) {
                nulls.push_back(true);
                indices.push_back(0);
            } else {
                nulls.push_back(false);
                auto s = v.ToString();
                if (dict.find(s) == dict.end()) {
                    dict[s] = static_cast<uint32_t>(dict_entries.size());
                    dict_entries.push_back(s);
                }
                indices.push_back(dict[s]);
            }
        }

        // Format: [dict_size][entries...][null_flags][indices...]
        uint32_t dict_size = static_cast<uint32_t>(dict_entries.size());
        auto *dp = reinterpret_cast<uint8_t *>(&dict_size);
        buf.data.insert(buf.data.end(), dp, dp + 4);

        for (auto &entry : dict_entries) {
            uint32_t len = static_cast<uint32_t>(entry.size());
            auto *lp = reinterpret_cast<uint8_t *>(&len);
            buf.data.insert(buf.data.end(), lp, lp + 4);
            buf.data.insert(buf.data.end(), entry.begin(), entry.end());
        }

        for (bool n : nulls) buf.data.push_back(n ? 1 : 0);
        for (auto idx : indices) {
            auto *ip = reinterpret_cast<uint8_t *>(&idx);
            buf.data.insert(buf.data.end(), ip, ip + 4);
        }

        return buf;
    }

    std::vector<Value> Decompress(const CompressedBuffer &buffer,
                                    const LogicalType &type) override {
        std::vector<Value> result;
        size_t pos = 0;
        auto &data = buffer.data;

        uint32_t dict_size;
        std::memcpy(&dict_size, &data[pos], 4); pos += 4;

        std::vector<std::string> dict_entries;
        for (uint32_t d = 0; d < dict_size; d++) {
            uint32_t len;
            std::memcpy(&len, &data[pos], 4); pos += 4;
            dict_entries.emplace_back(reinterpret_cast<const char *>(&data[pos]), len);
            pos += len;
        }

        std::vector<bool> nulls(buffer.count);
        for (idx_t i = 0; i < buffer.count; i++) {
            nulls[i] = data[pos++] != 0;
        }

        for (idx_t i = 0; i < buffer.count; i++) {
            if (nulls[i]) {
                result.push_back(Value());
            } else {
                uint32_t idx;
                std::memcpy(&idx, &data[pos], 4); pos += 4;
                auto &s = dict_entries[idx];
                switch (type.id()) {
                case LogicalTypeId::INTEGER: result.push_back(Value::INTEGER(std::stoi(s))); break;
                case LogicalTypeId::BIGINT: result.push_back(Value::BIGINT(std::stoll(s))); break;
                case LogicalTypeId::DOUBLE: result.push_back(Value::DOUBLE(std::stod(s))); break;
                case LogicalTypeId::VARCHAR: result.push_back(Value::VARCHAR(s)); break;
                default: result.push_back(Value::VARCHAR(s)); break;
                }
            }
        }
        return result;
    }

    CompressionType GetType() const override { return CompressionType::DICTIONARY; }
};

// ============================================================================
// Bitpacking Codec — pack small integers into minimum bits.
// ============================================================================

class BitpackingCodec : public CompressionCodec {
public:
    CompressedBuffer Compress(const std::vector<Value> &values,
                               const LogicalType &type) override {
        CompressedBuffer buf;
        buf.type = CompressionType::BITPACKING;
        buf.count = static_cast<idx_t>(values.size());

        // Find min value and range to determine bit width.
        int64_t min_val = std::numeric_limits<int64_t>::max();
        int64_t max_val = std::numeric_limits<int64_t>::min();
        std::vector<bool> nulls;

        for (auto &v : values) {
            if (v.IsNull()) {
                nulls.push_back(true);
                continue;
            }
            nulls.push_back(false);
            int64_t iv = 0;
            if (v.type().id() == LogicalTypeId::INTEGER) iv = v.GetValue<int32_t>();
            else if (v.type().id() == LogicalTypeId::BIGINT) iv = v.GetValue<int64_t>();
            min_val = std::min(min_val, iv);
            max_val = std::max(max_val, iv);
        }

        if (min_val > max_val) { min_val = 0; max_val = 0; }
        uint64_t range = static_cast<uint64_t>(max_val - min_val);
        uint8_t bit_width = 0;
        while ((1ULL << bit_width) <= range) bit_width++;
        if (bit_width == 0) bit_width = 1;

        // Store: min_val (8 bytes), bit_width (1 byte), null flags, packed bits.
        auto *mp = reinterpret_cast<uint8_t *>(&min_val);
        buf.data.insert(buf.data.end(), mp, mp + 8);
        buf.data.push_back(bit_width);

        for (bool n : nulls) buf.data.push_back(n ? 1 : 0);

        // Pack values using bit_width bits each.
        uint64_t bit_buffer = 0;
        int bits_used = 0;
        for (size_t i = 0; i < values.size(); i++) {
            if (nulls[i]) continue;
            int64_t iv = 0;
            if (values[i].type().id() == LogicalTypeId::INTEGER) iv = values[i].GetValue<int32_t>();
            else if (values[i].type().id() == LogicalTypeId::BIGINT) iv = values[i].GetValue<int64_t>();
            uint64_t encoded = static_cast<uint64_t>(iv - min_val);
            bit_buffer |= (encoded << bits_used);
            bits_used += bit_width;
            while (bits_used >= 8) {
                buf.data.push_back(static_cast<uint8_t>(bit_buffer & 0xFF));
                bit_buffer >>= 8;
                bits_used -= 8;
            }
        }
        if (bits_used > 0) {
            buf.data.push_back(static_cast<uint8_t>(bit_buffer & 0xFF));
        }

        return buf;
    }

    std::vector<Value> Decompress(const CompressedBuffer &buffer,
                                    const LogicalType &type) override {
        std::vector<Value> result;
        size_t pos = 0;
        auto &data = buffer.data;

        int64_t min_val;
        std::memcpy(&min_val, &data[pos], 8); pos += 8;
        uint8_t bit_width = data[pos++];

        std::vector<bool> nulls(buffer.count);
        for (idx_t i = 0; i < buffer.count; i++) {
            nulls[i] = data[pos++] != 0;
        }

        // Unpack bits.
        uint64_t bit_buffer = 0;
        int bits_available = 0;
        uint64_t mask = (1ULL << bit_width) - 1;

        for (idx_t i = 0; i < buffer.count; i++) {
            if (nulls[i]) {
                result.push_back(Value());
                continue;
            }
            while (bits_available < bit_width && pos < data.size()) {
                bit_buffer |= (static_cast<uint64_t>(data[pos++]) << bits_available);
                bits_available += 8;
            }
            uint64_t encoded = bit_buffer & mask;
            bit_buffer >>= bit_width;
            bits_available -= bit_width;
            int64_t val = min_val + static_cast<int64_t>(encoded);

            if (type.id() == LogicalTypeId::INTEGER)
                result.push_back(Value::INTEGER(static_cast<int32_t>(val)));
            else
                result.push_back(Value::BIGINT(val));
        }
        return result;
    }

    CompressionType GetType() const override { return CompressionType::BITPACKING; }
};

// ============================================================================
// Compression Manager — auto-select best codec.
// ============================================================================

CompressedBuffer CompressionManager::Compress(const std::vector<Value> &values,
                                               const LogicalType &type) {
    if (values.empty()) {
        CompressedBuffer buf;
        buf.type = CompressionType::UNCOMPRESSED;
        buf.count = 0;
        return buf;
    }

    auto zone_map = ComputeZoneMap(values);

    // Check if all non-null values are the same -> constant.
    bool all_same = true;
    Value first_val;
    bool found_first = false;
    for (auto &v : values) {
        if (v.IsNull()) continue;
        if (!found_first) { first_val = v; found_first = true; continue; }
        if (!(v == first_val)) { all_same = false; break; }
    }

    if (all_same && found_first) {
        ConstantCodec codec;
        auto buf = codec.Compress(values, type);
        buf.zone_map = zone_map;
        return buf;
    }

    // Check cardinality for dictionary encoding.
    std::unordered_set<std::string> unique_vals;
    for (auto &v : values) {
        if (!v.IsNull()) unique_vals.insert(v.ToString());
    }
    double cardinality_ratio = static_cast<double>(unique_vals.size()) / values.size();

    // Try all applicable codecs, pick smallest.
    struct Candidate {
        CompressionType type;
        size_t size;
        CompressedBuffer buffer;
    };
    std::vector<Candidate> candidates;

    // Uncompressed is always an option.
    {
        UncompressedCodec codec;
        auto buf = codec.Compress(values, type);
        candidates.push_back({CompressionType::UNCOMPRESSED, buf.data.size(), std::move(buf)});
    }

    // RLE: good when there are consecutive runs.
    {
        RLECodec codec;
        auto buf = codec.Compress(values, type);
        candidates.push_back({CompressionType::RLE, buf.data.size(), std::move(buf)});
    }

    // Dictionary: good when cardinality < 50%.
    if (cardinality_ratio < 0.5) {
        DictionaryCodec codec;
        auto buf = codec.Compress(values, type);
        candidates.push_back({CompressionType::DICTIONARY, buf.data.size(), std::move(buf)});
    }

    // Bitpacking: only for integer types.
    if (type.id() == LogicalTypeId::INTEGER || type.id() == LogicalTypeId::BIGINT) {
        BitpackingCodec codec;
        auto buf = codec.Compress(values, type);
        candidates.push_back({CompressionType::BITPACKING, buf.data.size(), std::move(buf)});
    }

    // Pick smallest.
    size_t best_idx = 0;
    for (size_t i = 1; i < candidates.size(); i++) {
        if (candidates[i].size < candidates[best_idx].size) {
            best_idx = i;
        }
    }

    auto result = std::move(candidates[best_idx].buffer);
    result.zone_map = zone_map;
    return result;
}

std::vector<Value> CompressionManager::Decompress(const CompressedBuffer &buffer,
                                                    const LogicalType &type) {
    switch (buffer.type) {
    case CompressionType::UNCOMPRESSED: {
        UncompressedCodec codec;
        return codec.Decompress(buffer, type);
    }
    case CompressionType::CONSTANT: {
        ConstantCodec codec;
        return codec.Decompress(buffer, type);
    }
    case CompressionType::RLE: {
        RLECodec codec;
        return codec.Decompress(buffer, type);
    }
    case CompressionType::DICTIONARY: {
        DictionaryCodec codec;
        return codec.Decompress(buffer, type);
    }
    case CompressionType::BITPACKING: {
        BitpackingCodec codec;
        return codec.Decompress(buffer, type);
    }
    }
    throw InternalException("Unknown compression type");
}

} // namespace slothdb

#pragma once

#include "slothdb/common/constants.hpp"
#include "slothdb/common/types/value.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace slothdb {

// Dictionary-encoded column: stores values as integer codes into a dictionary.
// Used for low-cardinality string columns to accelerate GROUP BY, JOIN, filter.
//
// Instead of comparing/hashing full strings, operations work on 32-bit integer codes.
// This gives 3-10x speedup on string-heavy analytical queries.

class DictionaryVector {
public:
    DictionaryVector() = default;

    // Build dictionary from a vector of values.
    void Build(const std::vector<Value> &values);

    // Encode a single value to its code. Returns INVALID_CODE for unknown values.
    static constexpr uint32_t INVALID_CODE = 0xFFFFFFFF;
    uint32_t Encode(const Value &val) const;

    // Decode a code back to its value.
    const Value &Decode(uint32_t code) const;

    // Get the integer code array (for vectorized operations).
    const std::vector<uint32_t> &GetCodes() const { return codes_; }
    std::vector<uint32_t> &GetCodes() { return codes_; }

    // Get dictionary entries.
    const std::vector<Value> &GetDictionary() const { return dictionary_; }

    // Get cardinality (number of unique values).
    idx_t Cardinality() const { return static_cast<idx_t>(dictionary_.size()); }

    // Get number of encoded values.
    idx_t Size() const { return static_cast<idx_t>(codes_.size()); }

    // Check if dictionary encoding is beneficial (cardinality < threshold).
    static bool ShouldEncode(idx_t num_values, idx_t cardinality) {
        // Encode if cardinality < 50% of total values.
        return cardinality * 2 < num_values;
    }

    // ============================================================
    // High-performance operations on codes (avoid string operations)
    // ============================================================

    // Group by: returns mapping from code -> list of row indices.
    std::unordered_map<uint32_t, std::vector<idx_t>> GroupByCodes() const;

    // Filter: returns indices where code matches.
    std::vector<idx_t> FilterEquals(uint32_t target_code) const;

    // Hash: compute hash of code (much faster than hashing strings).
    static uint32_t HashCode(uint32_t code) {
        // Murmur-inspired hash for 32-bit codes.
        code ^= code >> 16;
        code *= 0x45d9f3b;
        code ^= code >> 16;
        return code;
    }

    // Compare two codes (O(1) instead of O(n) string comparison).
    static bool CodesEqual(uint32_t a, uint32_t b) { return a == b; }

private:
    std::vector<Value> dictionary_;                      // code -> value
    std::unordered_map<std::string, uint32_t> encode_map_; // value_string -> code
    std::vector<uint32_t> codes_;                        // row -> code
    std::vector<bool> null_flags_;                       // row -> is_null
};

// Automatic dictionary encoding for DataChunks.
// Analyzes columns and creates dictionary vectors for low-cardinality ones.
class DictionaryEncoder {
public:
    // Analyze a column of values and decide whether to dictionary-encode.
    struct EncodingDecision {
        bool should_encode = false;
        idx_t cardinality = 0;
        idx_t num_values = 0;
        double cardinality_ratio = 1.0;
    };

    static EncodingDecision Analyze(const std::vector<Value> &values);

    // Encode a column. Returns nullptr if encoding is not beneficial.
    static std::unique_ptr<DictionaryVector> TryEncode(const std::vector<Value> &values);
};

} // namespace slothdb

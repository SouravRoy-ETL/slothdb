#include "doctest.h"
#include "slothdb/storage/compression/compression.hpp"

using namespace slothdb;

// ============================================================================
// Round-trip tests for each codec
// ============================================================================

TEST_CASE("Compression - Uncompressed round-trip") {
    std::vector<Value> values = {
        Value::INTEGER(1), Value::INTEGER(2), Value::INTEGER(3),
        Value(), Value::INTEGER(5)
    };

    auto buf = CompressionManager::Compress(values, LogicalType::INTEGER());
    auto result = CompressionManager::Decompress(buf, LogicalType::INTEGER());

    CHECK(result.size() == 5);
    CHECK(result[0].GetValue<int32_t>() == 1);
    CHECK(result[1].GetValue<int32_t>() == 2);
    CHECK(result[2].GetValue<int32_t>() == 3);
    CHECK(result[3].IsNull());
    CHECK(result[4].GetValue<int32_t>() == 5);
}

TEST_CASE("Compression - Constant codec (all same value)") {
    std::vector<Value> values(100, Value::INTEGER(42));

    auto buf = CompressionManager::Compress(values, LogicalType::INTEGER());
    CHECK(buf.type == CompressionType::CONSTANT);

    auto result = CompressionManager::Decompress(buf, LogicalType::INTEGER());
    CHECK(result.size() == 100);
    for (auto &v : result) {
        CHECK(v.GetValue<int32_t>() == 42);
    }

    // Constant should be much smaller than uncompressed.
    CHECK(buf.data.size() < 200);
}

TEST_CASE("Compression - RLE codec (runs)") {
    // Create data with runs: 5x1, 3x2, 4x3.
    std::vector<Value> values;
    for (int i = 0; i < 5; i++) values.push_back(Value::INTEGER(1));
    for (int i = 0; i < 3; i++) values.push_back(Value::INTEGER(2));
    for (int i = 0; i < 4; i++) values.push_back(Value::INTEGER(3));

    auto buf = CompressionManager::Compress(values, LogicalType::INTEGER());
    auto result = CompressionManager::Decompress(buf, LogicalType::INTEGER());

    CHECK(result.size() == 12);
    CHECK(result[0].GetValue<int32_t>() == 1);
    CHECK(result[4].GetValue<int32_t>() == 1);
    CHECK(result[5].GetValue<int32_t>() == 2);
    CHECK(result[8].GetValue<int32_t>() == 3);
}

TEST_CASE("Compression - Dictionary codec (low cardinality strings)") {
    std::vector<Value> values;
    for (int i = 0; i < 50; i++) {
        values.push_back(Value::VARCHAR(i % 3 == 0 ? "A" : (i % 3 == 1 ? "B" : "C")));
    }

    auto buf = CompressionManager::Compress(values, LogicalType::VARCHAR());
    CHECK(buf.type == CompressionType::DICTIONARY);

    auto result = CompressionManager::Decompress(buf, LogicalType::VARCHAR());
    CHECK(result.size() == 50);
    CHECK(result[0].GetValue<std::string>() == "A");
    CHECK(result[1].GetValue<std::string>() == "B");
    CHECK(result[2].GetValue<std::string>() == "C");
}

TEST_CASE("Compression - Bitpacking codec (small integers)") {
    // Values in range [0, 15] -> 4 bits each.
    std::vector<Value> values;
    for (int i = 0; i < 100; i++) {
        values.push_back(Value::INTEGER(i % 16));
    }

    auto buf = CompressionManager::Compress(values, LogicalType::INTEGER());
    // Bitpacking should be selected (or at least work).
    auto result = CompressionManager::Decompress(buf, LogicalType::INTEGER());

    CHECK(result.size() == 100);
    for (int i = 0; i < 100; i++) {
        CHECK(result[i].GetValue<int32_t>() == i % 16);
    }
}

TEST_CASE("Compression - Auto-selection picks best") {
    // Constant data -> should pick CONSTANT.
    {
        std::vector<Value> values(1000, Value::INTEGER(7));
        auto buf = CompressionManager::Compress(values, LogicalType::INTEGER());
        CHECK(buf.type == CompressionType::CONSTANT);
    }

    // Low cardinality strings -> should pick DICTIONARY or RLE (both valid).
    {
        std::vector<Value> values;
        // Mix values to prevent long runs, forcing DICTIONARY.
        for (int i = 0; i < 1000; i++) {
            int mod = i % 5;
            values.push_back(Value::VARCHAR(
                mod == 0 ? "alpha" : mod == 1 ? "beta" : mod == 2 ? "gamma"
                : mod == 3 ? "delta" : "epsilon"));
        }
        auto buf = CompressionManager::Compress(values, LogicalType::VARCHAR());
        CHECK((buf.type == CompressionType::DICTIONARY || buf.type == CompressionType::RLE));
        // Verify round-trip.
        auto result = CompressionManager::Decompress(buf, LogicalType::VARCHAR());
        CHECK(result.size() == 1000);
        CHECK(result[0].GetValue<std::string>() == "alpha");
    }
}

// ============================================================================
// Zone Map tests
// ============================================================================

TEST_CASE("ZoneMap - compute") {
    std::vector<Value> values = {
        Value::INTEGER(10), Value::INTEGER(5), Value::INTEGER(20),
        Value(), Value::INTEGER(15)
    };

    auto zm = CompressionManager::ComputeZoneMap(values);
    CHECK(zm.has_stats);
    CHECK(zm.count == 5);
    CHECK(zm.null_count == 1);
    CHECK(zm.min_value.GetValue<int32_t>() == 5);
    CHECK(zm.max_value.GetValue<int32_t>() == 20);
}

TEST_CASE("ZoneMap - MightContain") {
    ZoneMap zm;
    zm.min_value = Value::INTEGER(10);
    zm.max_value = Value::INTEGER(100);
    zm.has_stats = true;

    // x = 50 -> might contain (within range).
    CHECK(zm.MightContain("=", Value::INTEGER(50)) == true);
    // x = 200 -> can't contain (above max).
    CHECK(zm.MightContain("=", Value::INTEGER(200)) == false);
    // x = 5 -> can't contain (below min).
    CHECK(zm.MightContain("=", Value::INTEGER(5)) == false);

    // x > 50 -> might contain.
    CHECK(zm.MightContain(">", Value::INTEGER(50)) == true);
    // x > 100 -> can't contain (max is 100, need strictly >).
    CHECK(zm.MightContain(">", Value::INTEGER(100)) == false);
    // x > 200 -> can't contain.
    CHECK(zm.MightContain(">", Value::INTEGER(200)) == false);

    // x < 5 -> can't contain (min is 10).
    CHECK(zm.MightContain("<", Value::INTEGER(5)) == false);
    // x < 50 -> might contain.
    CHECK(zm.MightContain("<", Value::INTEGER(50)) == true);
}

TEST_CASE("Compression - handles nulls correctly") {
    std::vector<Value> values = {Value(), Value(), Value::INTEGER(42), Value()};

    auto buf = CompressionManager::Compress(values, LogicalType::INTEGER());
    auto result = CompressionManager::Decompress(buf, LogicalType::INTEGER());

    CHECK(result.size() == 4);
    CHECK(result[0].IsNull());
    CHECK(result[1].IsNull());
    CHECK(result[2].GetValue<int32_t>() == 42);
    CHECK(result[3].IsNull());
}

TEST_CASE("Compression - empty input") {
    std::vector<Value> values;
    auto buf = CompressionManager::Compress(values, LogicalType::INTEGER());
    CHECK(buf.count == 0);

    auto result = CompressionManager::Decompress(buf, LogicalType::INTEGER());
    CHECK(result.empty());
}

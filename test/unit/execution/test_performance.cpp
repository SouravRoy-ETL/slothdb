#include "doctest.h"
#include "slothdb/execution/dictionary_vector.hpp"
#include "slothdb/execution/gpu/gpu_engine.hpp"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include <chrono>

using namespace slothdb;

// ============================================================================
// Dictionary Encoding
// ============================================================================

TEST_CASE("DictionaryVector - build and encode") {
    std::vector<Value> values = {
        Value::VARCHAR("A"), Value::VARCHAR("B"), Value::VARCHAR("A"),
        Value::VARCHAR("C"), Value::VARCHAR("B"), Value::VARCHAR("A"),
    };

    DictionaryVector dv;
    dv.Build(values);

    CHECK(dv.Cardinality() == 3);  // A, B, C
    CHECK(dv.Size() == 6);

    // Codes should be consistent.
    auto &codes = dv.GetCodes();
    CHECK(codes[0] == codes[2]); // A == A
    CHECK(codes[0] == codes[5]); // A == A
    CHECK(codes[1] == codes[4]); // B == B
    CHECK(codes[0] != codes[1]); // A != B
}

TEST_CASE("DictionaryVector - decode") {
    std::vector<Value> values = {
        Value::VARCHAR("hello"), Value::VARCHAR("world"), Value::VARCHAR("hello"),
    };

    DictionaryVector dv;
    dv.Build(values);

    CHECK(dv.Decode(dv.GetCodes()[0]).GetValue<std::string>() == "hello");
    CHECK(dv.Decode(dv.GetCodes()[1]).GetValue<std::string>() == "world");
}

TEST_CASE("DictionaryVector - null handling") {
    std::vector<Value> values = {
        Value::VARCHAR("A"), Value(), Value::VARCHAR("A"),
    };

    DictionaryVector dv;
    dv.Build(values);

    CHECK(dv.Cardinality() == 1);  // Only "A"
    CHECK(dv.GetCodes()[1] == DictionaryVector::INVALID_CODE); // NULL
}

TEST_CASE("DictionaryVector - group by codes") {
    std::vector<Value> values = {
        Value::VARCHAR("X"), Value::VARCHAR("Y"), Value::VARCHAR("X"),
        Value::VARCHAR("Y"), Value::VARCHAR("X"),
    };

    DictionaryVector dv;
    dv.Build(values);
    auto groups = dv.GroupByCodes();

    // Should have 2 groups.
    CHECK(groups.size() == 2);

    // X appears at indices 0, 2, 4.
    auto x_code = dv.Encode(Value::VARCHAR("X"));
    CHECK(groups[x_code].size() == 3);
}

TEST_CASE("DictionaryVector - filter equals") {
    std::vector<Value> values;
    for (int i = 0; i < 1000; i++) {
        values.push_back(Value::VARCHAR(i % 3 == 0 ? "match" : "other"));
    }

    DictionaryVector dv;
    dv.Build(values);
    auto match_code = dv.Encode(Value::VARCHAR("match"));
    auto indices = dv.FilterEquals(match_code);

    CHECK(indices.size() == 334); // ceil(1000/3)
}

TEST_CASE("DictionaryEncoder - should encode low cardinality") {
    std::vector<Value> values;
    for (int i = 0; i < 10000; i++) {
        values.push_back(Value::VARCHAR(std::to_string(i % 5)));
    }

    auto decision = DictionaryEncoder::Analyze(values);
    CHECK(decision.should_encode == true);
    CHECK(decision.cardinality == 5);
}

TEST_CASE("DictionaryEncoder - should NOT encode high cardinality") {
    std::vector<Value> values;
    for (int i = 0; i < 100; i++) {
        values.push_back(Value::VARCHAR("unique_" + std::to_string(i)));
    }

    auto decision = DictionaryEncoder::Analyze(values);
    CHECK(decision.should_encode == false);
}

// ============================================================================
// GPU Engine (CPU Fallback)
// ============================================================================

TEST_CASE("GPUEngine - SumInt32") {
    auto engine = GPUEngine::Create();

    std::vector<int32_t> data(10000);
    for (int i = 0; i < 10000; i++) data[i] = i + 1;

    auto sum = engine->SumInt32(data.data(), 10000);
    CHECK(sum == 50005000); // sum(1..10000)
}

TEST_CASE("GPUEngine - SumDouble") {
    auto engine = GPUEngine::Create();

    std::vector<double> data = {1.5, 2.5, 3.0};
    auto sum = engine->SumDouble(data.data(), 3);
    CHECK(sum == doctest::Approx(7.0));
}

TEST_CASE("GPUEngine - Filter") {
    auto engine = GPUEngine::Create();

    std::vector<int32_t> data = {1, 5, 3, 8, 2, 9, 4, 7, 6, 10};
    auto result = engine->Filter(data.data(), 10, ">", 5);

    // >5: values 8,9,7,6,10 at indices 3,5,7,8,9
    CHECK(result.size() == 5);
}

TEST_CASE("GPUEngine - SortIndices") {
    auto engine = GPUEngine::Create();

    std::vector<int32_t> data = {5, 3, 8, 1, 9};
    auto indices = engine->SortIndices(data.data(), 5, true);

    CHECK(indices[0] == 3); // data[3] = 1 (smallest)
    CHECK(indices[4] == 4); // data[4] = 9 (largest)
}

TEST_CASE("GPUEngine - HashAggregate") {
    auto engine = GPUEngine::Create();

    std::vector<int32_t> keys = {1, 2, 1, 2, 1};
    std::vector<int32_t> values = {10, 20, 30, 40, 50};

    auto result = engine->HashAggregate(keys.data(), values.data(), 5);

    CHECK(result.keys.size() == 2);

    for (size_t i = 0; i < result.keys.size(); i++) {
        if (result.keys[i] == 1) {
            CHECK(result.sums[i] == 90);  // 10+30+50
            CHECK(result.counts[i] == 3);
        }
        if (result.keys[i] == 2) {
            CHECK(result.sums[i] == 60);  // 20+40
            CHECK(result.counts[i] == 2);
        }
    }
}

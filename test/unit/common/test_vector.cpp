#include "doctest.h"
#include "slothdb/common/types/vector.hpp"

using namespace slothdb;

TEST_CASE("Vector - INTEGER set/get") {
    Vector vec(LogicalType::INTEGER());
    for (idx_t i = 0; i < VECTOR_SIZE; i++) {
        vec.SetValue(i, Value::INTEGER(static_cast<int32_t>(i * 10)));
    }
    for (idx_t i = 0; i < VECTOR_SIZE; i++) {
        auto val = vec.GetValue(i);
        CHECK(val.GetValue<int32_t>() == static_cast<int32_t>(i * 10));
    }
}

TEST_CASE("Vector - DOUBLE set/get") {
    Vector vec(LogicalType::DOUBLE());
    vec.SetValue(0, Value::DOUBLE(3.14));
    vec.SetValue(1, Value::DOUBLE(2.718));
    CHECK(vec.GetValue(0).GetValue<double>() == doctest::Approx(3.14));
    CHECK(vec.GetValue(1).GetValue<double>() == doctest::Approx(2.718));
}

TEST_CASE("Vector - BOOLEAN set/get") {
    Vector vec(LogicalType::BOOLEAN());
    vec.SetValue(0, Value::BOOLEAN(true));
    vec.SetValue(1, Value::BOOLEAN(false));
    CHECK(vec.GetValue(0).GetValue<bool>() == true);
    CHECK(vec.GetValue(1).GetValue<bool>() == false);
}

TEST_CASE("Vector - NULL handling") {
    Vector vec(LogicalType::INTEGER());
    vec.SetValue(0, Value::INTEGER(42));
    vec.SetValue(1, Value());  // NULL
    vec.SetValue(2, Value::INTEGER(99));

    CHECK_FALSE(vec.GetValue(0).IsNull());
    CHECK(vec.GetValue(1).IsNull());
    CHECK_FALSE(vec.GetValue(2).IsNull());
    CHECK(vec.GetValue(0).GetValue<int32_t>() == 42);
    CHECK(vec.GetValue(2).GetValue<int32_t>() == 99);
}

TEST_CASE("Vector - VARCHAR short strings (inlined)") {
    Vector vec(LogicalType::VARCHAR());
    vec.SetValue(0, Value::VARCHAR("hello"));
    vec.SetValue(1, Value::VARCHAR("world"));
    vec.SetValue(2, Value::VARCHAR(""));

    CHECK(vec.GetValue(0).GetValue<std::string>() == "hello");
    CHECK(vec.GetValue(1).GetValue<std::string>() == "world");
    CHECK(vec.GetValue(2).GetValue<std::string>() == "");
}

TEST_CASE("Vector - VARCHAR long strings (overflow)") {
    Vector vec(LogicalType::VARCHAR());
    std::string long_str = "this is a long string that exceeds inline storage capacity";
    vec.SetValue(0, Value::VARCHAR(long_str));
    vec.SetValue(1, Value::VARCHAR("short"));

    CHECK(vec.GetValue(0).GetValue<std::string>() == long_str);
    CHECK(vec.GetValue(1).GetValue<std::string>() == "short");
}

TEST_CASE("Vector - CONSTANT flatten") {
    Vector vec(LogicalType::INTEGER());
    vec.SetVectorType(VectorType::CONSTANT);
    vec.SetValue(0, Value::INTEGER(42));

    // Before flatten: constant mode.
    CHECK(vec.GetVectorType() == VectorType::CONSTANT);

    // Flatten to 100 rows.
    vec.Flatten(100);
    CHECK(vec.GetVectorType() == VectorType::FLAT);

    // All 100 rows should have value 42.
    for (idx_t i = 0; i < 100; i++) {
        auto val = vec.GetValue(i);
        CHECK(val.GetValue<int32_t>() == 42);
    }
}

TEST_CASE("Vector - CONSTANT null flatten") {
    Vector vec(LogicalType::INTEGER());
    vec.SetVectorType(VectorType::CONSTANT);
    vec.SetValue(0, Value()); // NULL constant

    vec.Flatten(50);
    for (idx_t i = 0; i < 50; i++) {
        CHECK(vec.GetValue(i).IsNull());
    }
}

TEST_CASE("Vector - move semantics") {
    Vector v1(LogicalType::INTEGER());
    v1.SetValue(0, Value::INTEGER(42));

    Vector v2(std::move(v1));
    CHECK(v2.GetValue(0).GetValue<int32_t>() == 42);
    CHECK(v2.GetType().id() == LogicalTypeId::INTEGER);
}

TEST_CASE("Vector - BIGINT") {
    Vector vec(LogicalType::BIGINT());
    vec.SetValue(0, Value::BIGINT(1234567890123LL));
    CHECK(vec.GetValue(0).GetValue<int64_t>() == 1234567890123LL);
}

TEST_CASE("Vector - unsigned types") {
    Vector vec(LogicalType::UINTEGER());
    vec.SetValue(0, Value::UINTEGER(4000000000U));
    CHECK(vec.GetValue(0).GetValue<uint32_t>() == 4000000000U);
}

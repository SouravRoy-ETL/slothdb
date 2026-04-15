#include "doctest.h"
#include "slothdb/common/types/value.hpp"

using namespace slothdb;

TEST_CASE("Value - NULL") {
    Value v;
    CHECK(v.IsNull());
    CHECK(v.ToString() == "NULL");
}

TEST_CASE("Value - INTEGER") {
    auto v = Value::INTEGER(42);
    CHECK_FALSE(v.IsNull());
    CHECK(v.GetValue<int32_t>() == 42);
    CHECK(v.ToString() == "42");
    CHECK(v.type().id() == LogicalTypeId::INTEGER);
}

TEST_CASE("Value - BIGINT") {
    auto v = Value::BIGINT(1234567890123LL);
    CHECK(v.GetValue<int64_t>() == 1234567890123LL);
    CHECK(v.type().id() == LogicalTypeId::BIGINT);
}

TEST_CASE("Value - FLOAT and DOUBLE") {
    auto f = Value::FLOAT(3.14f);
    CHECK(f.GetValue<float>() == doctest::Approx(3.14f));

    auto d = Value::DOUBLE(2.718281828);
    CHECK(d.GetValue<double>() == doctest::Approx(2.718281828));
}

TEST_CASE("Value - BOOLEAN") {
    auto t = Value::BOOLEAN(true);
    CHECK(t.GetValue<bool>() == true);
    CHECK(t.ToString() == "true");

    auto f = Value::BOOLEAN(false);
    CHECK(f.GetValue<bool>() == false);
    CHECK(f.ToString() == "false");
}

TEST_CASE("Value - VARCHAR") {
    auto v = Value::VARCHAR("hello");
    CHECK_FALSE(v.IsNull());
    CHECK(v.GetValue<std::string>() == "hello");
    CHECK(v.ToString() == "hello");
    CHECK(v.type().id() == LogicalTypeId::VARCHAR);
}

TEST_CASE("Value - equality") {
    CHECK(Value::INTEGER(10) == Value::INTEGER(10));
    CHECK(Value::INTEGER(10) != Value::INTEGER(20));
    CHECK(Value::VARCHAR("abc") == Value::VARCHAR("abc"));
    CHECK(Value::VARCHAR("abc") != Value::VARCHAR("xyz"));

    // Two NULLs are equal.
    CHECK(Value() == Value());
}

TEST_CASE("Value - copy and move") {
    auto v1 = Value::INTEGER(42);
    auto v2 = v1; // copy
    CHECK(v2.GetValue<int32_t>() == 42);

    auto v3 = std::move(v1); // move
    CHECK(v3.GetValue<int32_t>() == 42);
}

TEST_CASE("Value - HUGEINT") {
    auto v = Value::HUGEINT(hugeint_t(0, 12345));
    CHECK(v.GetValue<hugeint_t>() == hugeint_t(0, 12345));
    CHECK(v.type().id() == LogicalTypeId::HUGEINT);
}

TEST_CASE("Value - unsigned types") {
    auto u8 = Value::UTINYINT(255);
    CHECK(u8.GetValue<uint8_t>() == 255);

    auto u16 = Value::USMALLINT(65535);
    CHECK(u16.GetValue<uint16_t>() == 65535);

    auto u32 = Value::UINTEGER(4000000000U);
    CHECK(u32.GetValue<uint32_t>() == 4000000000U);

    auto u64 = Value::UBIGINT(18000000000000000000ULL);
    CHECK(u64.GetValue<uint64_t>() == 18000000000000000000ULL);
}

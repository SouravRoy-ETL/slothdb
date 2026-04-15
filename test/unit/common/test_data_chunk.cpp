#include "doctest.h"
#include "slothdb/common/types/data_chunk.hpp"

using namespace slothdb;

TEST_CASE("DataChunk - initialize and basic properties") {
    DataChunk chunk;
    chunk.Initialize({LogicalType::INTEGER(), LogicalType::VARCHAR(), LogicalType::DOUBLE()});

    CHECK(chunk.ColumnCount() == 3);
    CHECK(chunk.size() == 0);
}

TEST_CASE("DataChunk - set/get values") {
    DataChunk chunk;
    chunk.Initialize({LogicalType::INTEGER(), LogicalType::VARCHAR(), LogicalType::DOUBLE()});

    for (idx_t i = 0; i < 1000; i++) {
        chunk.SetValue(0, i, Value::INTEGER(static_cast<int32_t>(i)));
        chunk.SetValue(1, i, Value::VARCHAR("row_" + std::to_string(i)));
        chunk.SetValue(2, i, Value::DOUBLE(static_cast<double>(i) * 1.5));
    }

    CHECK(chunk.size() == 1000);

    // Verify a few rows.
    CHECK(chunk.GetValue(0, 0).GetValue<int32_t>() == 0);
    CHECK(chunk.GetValue(1, 0).GetValue<std::string>() == "row_0");
    CHECK(chunk.GetValue(2, 0).GetValue<double>() == doctest::Approx(0.0));

    CHECK(chunk.GetValue(0, 999).GetValue<int32_t>() == 999);
    CHECK(chunk.GetValue(1, 999).GetValue<std::string>() == "row_999");
    CHECK(chunk.GetValue(2, 999).GetValue<double>() == doctest::Approx(999 * 1.5));
}

TEST_CASE("DataChunk - null values") {
    DataChunk chunk;
    chunk.Initialize({LogicalType::INTEGER()});

    chunk.SetValue(0, 0, Value::INTEGER(10));
    chunk.SetValue(0, 1, Value()); // NULL
    chunk.SetValue(0, 2, Value::INTEGER(30));

    CHECK_FALSE(chunk.GetValue(0, 0).IsNull());
    CHECK(chunk.GetValue(0, 1).IsNull());
    CHECK_FALSE(chunk.GetValue(0, 2).IsNull());
}

TEST_CASE("DataChunk - Reset") {
    DataChunk chunk;
    chunk.Initialize({LogicalType::INTEGER()});
    chunk.SetValue(0, 0, Value::INTEGER(42));
    CHECK(chunk.size() == 1);

    chunk.Reset();
    CHECK(chunk.size() == 0);
    CHECK(chunk.ColumnCount() == 1); // structure preserved
}

TEST_CASE("DataChunk - Append") {
    DataChunk c1;
    c1.Initialize({LogicalType::INTEGER(), LogicalType::VARCHAR()});
    c1.SetValue(0, 0, Value::INTEGER(1));
    c1.SetValue(1, 0, Value::VARCHAR("a"));
    c1.SetValue(0, 1, Value::INTEGER(2));
    c1.SetValue(1, 1, Value::VARCHAR("b"));

    DataChunk c2;
    c2.Initialize({LogicalType::INTEGER(), LogicalType::VARCHAR()});
    c2.SetValue(0, 0, Value::INTEGER(3));
    c2.SetValue(1, 0, Value::VARCHAR("c"));

    c1.Append(c2);
    CHECK(c1.size() == 3);
    CHECK(c1.GetValue(0, 2).GetValue<int32_t>() == 3);
    CHECK(c1.GetValue(1, 2).GetValue<std::string>() == "c");
}

TEST_CASE("DataChunk - ToString") {
    DataChunk chunk;
    chunk.Initialize({LogicalType::INTEGER(), LogicalType::VARCHAR()});
    chunk.SetValue(0, 0, Value::INTEGER(42));
    chunk.SetValue(1, 0, Value::VARCHAR("hello"));

    auto str = chunk.ToString();
    CHECK(str.find("42") != std::string::npos);
    CHECK(str.find("hello") != std::string::npos);
}

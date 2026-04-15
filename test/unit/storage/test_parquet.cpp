#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/storage/parquet.hpp"
#include <cstdio>

using namespace slothdb;

static const char *TEST_PARQUET = "test_data.parquet";

static void cleanup() { std::remove(TEST_PARQUET); }

TEST_CASE("Parquet - write and read basic types") {
    cleanup();

    // Write.
    {
        ParquetWriter writer(TEST_PARQUET,
            {"id", "name", "score"},
            {LogicalType::INTEGER(), LogicalType::VARCHAR(), LogicalType::DOUBLE()});

        std::vector<std::vector<Value>> rows = {
            {Value::INTEGER(1), Value::VARCHAR("Alice"), Value::DOUBLE(95.5)},
            {Value::INTEGER(2), Value::VARCHAR("Bob"), Value::DOUBLE(87.3)},
            {Value::INTEGER(3), Value::VARCHAR("Charlie"), Value::DOUBLE(92.1)},
        };
        writer.WriteRowGroup(rows);
        writer.Finish();
    }

    // Read.
    {
        ParquetReader reader(TEST_PARQUET);
        CHECK(reader.NumRows() == 3);
        CHECK(reader.GetColumnNames().size() == 3);
        CHECK(reader.GetColumnNames()[0] == "id");
        CHECK(reader.GetColumnNames()[1] == "name");

        auto rows = reader.ReadAll();
        CHECK(rows.size() == 3);
        CHECK(rows[0][0].GetValue<int32_t>() == 1);
        CHECK(rows[0][1].GetValue<std::string>() == "Alice");
        CHECK(rows[0][2].GetValue<double>() == doctest::Approx(95.5));
        CHECK(rows[2][1].GetValue<std::string>() == "Charlie");
    }

    cleanup();
}

TEST_CASE("Parquet - null values") {
    cleanup();

    {
        ParquetWriter writer(TEST_PARQUET,
            {"x", "y"}, {LogicalType::INTEGER(), LogicalType::VARCHAR()});

        std::vector<std::vector<Value>> rows = {
            {Value::INTEGER(1), Value::VARCHAR("hello")},
            {Value(), Value::VARCHAR("world")},
            {Value::INTEGER(3), Value()},
        };
        writer.WriteRowGroup(rows);
        writer.Finish();
    }

    {
        ParquetReader reader(TEST_PARQUET);
        auto rows = reader.ReadAll();
        CHECK(rows.size() == 3);
        CHECK_FALSE(rows[0][0].IsNull());
        CHECK(rows[1][0].IsNull());
        CHECK(rows[2][1].IsNull());
    }

    cleanup();
}

TEST_CASE("Parquet - row group statistics") {
    cleanup();

    {
        ParquetWriter writer(TEST_PARQUET,
            {"x"}, {LogicalType::INTEGER()});

        std::vector<std::vector<Value>> rows;
        for (int i = 10; i <= 100; i += 10) {
            rows.push_back({Value::INTEGER(i)});
        }
        writer.WriteRowGroup(rows);
        writer.Finish();
    }

    {
        ParquetReader reader(TEST_PARQUET);
        // x = 50 should match (within [10, 100]).
        CHECK(reader.RowGroupMightMatch(0, 0, "=", Value::INTEGER(50)));
        // x = 200 should NOT match (above max).
        CHECK_FALSE(reader.RowGroupMightMatch(0, 0, "=", Value::INTEGER(200)));
        // x > 50 should match.
        CHECK(reader.RowGroupMightMatch(0, 0, ">", Value::INTEGER(50)));
        // x < 5 should NOT match (below min).
        CHECK_FALSE(reader.RowGroupMightMatch(0, 0, "<", Value::INTEGER(5)));
    }

    cleanup();
}

TEST_CASE("Parquet - COPY TO and FROM") {
    cleanup();

    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR, val DOUBLE)");
    conn.Query("INSERT INTO t VALUES (1, 'A', 1.1), (2, 'B', 2.2), (3, 'C', 3.3)");

    conn.Query("COPY t TO 'test_data.parquet' WITH (FORMAT PARQUET)");

    conn.Query("CREATE TABLE t2 (id INTEGER, name VARCHAR, val DOUBLE)");
    conn.Query("COPY t2 FROM 'test_data.parquet' WITH (FORMAT PARQUET)");

    auto r = conn.Query("SELECT COUNT(*) FROM t2");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 3);

    auto r2 = conn.Query("SELECT name FROM t2 WHERE id = 2");
    CHECK(r2.GetValue(0, 0).GetValue<std::string>() == "B");

    cleanup();
}

TEST_CASE("Parquet - read_parquet() table function") {
    cleanup();

    // Write a parquet file first.
    {
        ParquetWriter writer(TEST_PARQUET,
            {"x", "y"}, {LogicalType::INTEGER(), LogicalType::VARCHAR()});
        writer.WriteRowGroup({
            {Value::INTEGER(42), Value::VARCHAR("hello")},
            {Value::INTEGER(99), Value::VARCHAR("world")},
        });
        writer.Finish();
    }

    Database db;
    Connection conn(db);
    // Simple test first.
    auto r0 = conn.Query("SELECT 1");
    CHECK(r0.RowCount() == 1);

    auto r = conn.Query("SELECT * FROM read_parquet('test_data.parquet')");
    CHECK(r.RowCount() == 2);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 42);
    CHECK(r.GetValue(1, 1).GetValue<std::string>() == "world");

    cleanup();
}

TEST_CASE("Parquet - auto-detect from file extension") {
    cleanup();

    {
        ParquetWriter writer(TEST_PARQUET,
            {"val"}, {LogicalType::INTEGER()});
        writer.WriteRowGroup({{Value::INTEGER(7)}, {Value::INTEGER(8)}});
        writer.Finish();
    }

    Database db;
    Connection conn(db);
    auto r = conn.Query("SELECT * FROM 'test_data.parquet'");
    CHECK(r.RowCount() == 2);

    cleanup();
}

TEST_CASE("Parquet - round-trip preserves data") {
    cleanup();

    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE src (a INTEGER, b VARCHAR, c DOUBLE)");
    conn.Query("INSERT INTO src VALUES (1, 'x', 1.5), (2, 'y', 2.5)");
    conn.Query("COPY src TO 'test_data.parquet' WITH (FORMAT PARQUET)");

    auto r = conn.Query("SELECT a, b, c FROM read_parquet('test_data.parquet')");
    CHECK(r.RowCount() == 2);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 1);
    CHECK(r.GetValue(0, 1).GetValue<std::string>() == "x");
    CHECK(r.GetValue(0, 2).GetValue<double>() == doctest::Approx(1.5));

    cleanup();
}

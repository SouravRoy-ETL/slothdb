#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/storage/json_reader.hpp"
#include <cstdio>
#include <fstream>

using namespace slothdb;

static void cleanup_files() {
    std::remove("test.json");
    std::remove("test.ndjson");
    std::remove("test_out.json");
    std::remove("test_data2.csv");
}

// ============================================================================
// JSON Reader/Writer
// ============================================================================

TEST_CASE("JSON - read JSON array") {
    {
        std::ofstream f("test.json");
        f << R"([
  {"id": 1, "name": "Alice", "score": 95.5},
  {"id": 2, "name": "Bob", "score": 87.3},
  {"id": 3, "name": "Charlie", "score": 92.1}
])";
    }

    JSONReader reader("test.json");
    reader.DetectSchema();
    auto names = reader.GetColumnNames();
    auto types = reader.GetColumnTypes();
    auto rows = reader.ReadAll();

    CHECK(names.size() == 3);
    CHECK(rows.size() == 3);
    CHECK(rows[0][0].GetValue<int32_t>() == 1);
    CHECK(rows[0][1].GetValue<std::string>() == "Alice");

    cleanup_files();
}

TEST_CASE("JSON - read NDJSON") {
    {
        std::ofstream f("test.ndjson");
        f << R"({"x": 10, "y": "hello"})" << "\n";
        f << R"({"x": 20, "y": "world"})" << "\n";
    }

    JSONOptions opts;
    opts.array_format = false;
    JSONReader reader("test.ndjson", opts);
    reader.DetectSchema();
    auto rows = reader.ReadAll();
    CHECK(rows.size() == 2);
    CHECK(rows[0][0].GetValue<int32_t>() == 10);
    CHECK(rows[1][1].GetValue<std::string>() == "world");

    cleanup_files();
}

TEST_CASE("JSON Writer - write and read back") {
    {
        JSONWriter writer("test_out.json");
        writer.WriteHeader({"id", "name"});
        writer.WriteRow({"id", "name"}, {Value::INTEGER(1), Value::VARCHAR("Alice")});
        writer.WriteRow({"id", "name"}, {Value::INTEGER(2), Value::VARCHAR("Bob")});
        writer.Finish();
    }

    JSONReader reader("test_out.json");
    reader.DetectSchema();
    auto rows = reader.ReadAll();
    CHECK(rows.size() == 2);

    cleanup_files();
}

// ============================================================================
// read_json() table function
// ============================================================================

TEST_CASE("read_json - query JSON file directly") {
    {
        std::ofstream f("test.json");
        f << R"([{"id": 1, "name": "Alice"}, {"id": 2, "name": "Bob"}])";
    }

    Database db;
    Connection conn(db);
    auto r = conn.Query("SELECT * FROM read_json('test.json')");
    CHECK(r.RowCount() == 2);
    CHECK(r.ColumnCount() == 2);

    cleanup_files();
}

TEST_CASE("read_json - with WHERE filter") {
    {
        std::ofstream f("test.json");
        f << R"([{"x": 1}, {"x": 2}, {"x": 3}, {"x": 4}, {"x": 5}])";
    }

    Database db;
    Connection conn(db);
    auto r = conn.Query("SELECT x FROM read_json('test.json') WHERE x > 3");
    CHECK(r.RowCount() == 2);

    cleanup_files();
}

// ============================================================================
// read_csv() table function
// ============================================================================

TEST_CASE("read_csv - query CSV file directly") {
    {
        std::ofstream f("test_data2.csv");
        f << "a,b\n1,hello\n2,world\n";
    }

    Database db;
    Connection conn(db);
    auto r = conn.Query("SELECT * FROM read_csv('test_data2.csv')");
    CHECK(r.RowCount() == 2);
    CHECK(r.ColumnCount() == 2);

    cleanup_files();
}

// ============================================================================
// Auto-detect: SELECT * FROM 'file.csv'
// ============================================================================

TEST_CASE("Auto-detect - CSV from string literal") {
    {
        std::ofstream f("test_data2.csv");
        f << "x,y\n10,hello\n20,world\n";
    }

    Database db;
    Connection conn(db);
    auto r = conn.Query("SELECT * FROM 'test_data2.csv'");
    CHECK(r.RowCount() == 2);

    cleanup_files();
}

TEST_CASE("Auto-detect - JSON from string literal") {
    {
        std::ofstream f("test.json");
        f << R"([{"val": 42}])";
    }

    Database db;
    Connection conn(db);
    auto r = conn.Query("SELECT * FROM 'test.json'");
    CHECK(r.RowCount() == 1);

    cleanup_files();
}

// ============================================================================
// COPY with FORMAT JSON
// ============================================================================

TEST_CASE("COPY TO JSON format") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR)");
    conn.Query("INSERT INTO t VALUES (1, 'Alice'), (2, 'Bob')");

    conn.Query("COPY t TO 'test_out.json' WITH (FORMAT JSON)");

    // Read it back.
    JSONReader reader("test_out.json");
    reader.DetectSchema();
    auto rows = reader.ReadAll();
    CHECK(rows.size() == 2);

    cleanup_files();
}

TEST_CASE("COPY FROM JSON format") {
    {
        std::ofstream f("test.json");
        f << R"([{"id": 10, "name": "X"}, {"id": 20, "name": "Y"}])";
    }

    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR)");
    conn.Query("COPY t FROM 'test.json' WITH (FORMAT JSON)");

    auto r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 2);

    cleanup_files();
}

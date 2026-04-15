#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/common/exception.hpp"

using namespace slothdb;

TEST_CASE("E2E - CREATE TABLE and INSERT via SQL") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t1 (id INTEGER, name VARCHAR, score DOUBLE)");
    conn.Query("INSERT INTO t1 VALUES (1, 'Alice', 95.5)");
    conn.Query("INSERT INTO t1 VALUES (2, 'Bob', 87.3)");
    conn.Query("INSERT INTO t1 VALUES (3, 'Charlie', 92.1)");

    auto result = conn.Query("SELECT * FROM t1");
    CHECK(result.RowCount() == 3);
    CHECK(result.ColumnCount() == 3);
    CHECK(result.column_names[0] == "id");
    CHECK(result.column_names[1] == "name");
    CHECK(result.column_names[2] == "score");
}

TEST_CASE("E2E - SELECT specific columns") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (a INTEGER, b VARCHAR, c DOUBLE)");
    conn.Query("INSERT INTO t VALUES (10, 'x', 1.1)");
    conn.Query("INSERT INTO t VALUES (20, 'y', 2.2)");

    auto result = conn.Query("SELECT a, c FROM t");
    CHECK(result.RowCount() == 2);
    CHECK(result.ColumnCount() == 2);
    CHECK(result.GetValue(0, 0).GetValue<int32_t>() == 10);
    CHECK(result.GetValue(0, 1).GetValue<double>() == doctest::Approx(1.1));
    CHECK(result.GetValue(1, 0).GetValue<int32_t>() == 20);
}

TEST_CASE("E2E - SELECT with WHERE filter") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE nums (x INTEGER)");
    conn.Query("INSERT INTO nums VALUES (1)");
    conn.Query("INSERT INTO nums VALUES (2)");
    conn.Query("INSERT INTO nums VALUES (3)");
    conn.Query("INSERT INTO nums VALUES (4)");
    conn.Query("INSERT INTO nums VALUES (5)");

    auto result = conn.Query("SELECT x FROM nums WHERE x > 3");
    CHECK(result.RowCount() == 2);
    CHECK(result.GetValue(0, 0).GetValue<int32_t>() == 4);
    CHECK(result.GetValue(1, 0).GetValue<int32_t>() == 5);
}

TEST_CASE("E2E - SELECT with WHERE on strings") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE people (name VARCHAR, age INTEGER)");
    conn.Query("INSERT INTO people VALUES ('Alice', 30)");
    conn.Query("INSERT INTO people VALUES ('Bob', 25)");
    conn.Query("INSERT INTO people VALUES ('Charlie', 35)");

    auto result = conn.Query("SELECT name FROM people WHERE age > 28");
    CHECK(result.RowCount() == 2);
}

TEST_CASE("E2E - SELECT with LIMIT") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER)");
    for (int i = 1; i <= 10; i++) {
        conn.Query("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    }

    auto result = conn.Query("SELECT x FROM t LIMIT 3");
    CHECK(result.RowCount() == 3);
}

TEST_CASE("E2E - SELECT with LIMIT and OFFSET") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER)");
    for (int i = 1; i <= 10; i++) {
        conn.Query("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    }

    auto result = conn.Query("SELECT x FROM t LIMIT 3 OFFSET 2");
    CHECK(result.RowCount() == 3);
    CHECK(result.GetValue(0, 0).GetValue<int32_t>() == 3);
    CHECK(result.GetValue(1, 0).GetValue<int32_t>() == 4);
    CHECK(result.GetValue(2, 0).GetValue<int32_t>() == 5);
}

TEST_CASE("E2E - SELECT with arithmetic") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (a INTEGER, b INTEGER)");
    conn.Query("INSERT INTO t VALUES (10, 3)");

    auto result = conn.Query("SELECT a + b, a * b, a - b FROM t");
    CHECK(result.RowCount() == 1);
    CHECK(result.GetValue(0, 0).GetValue<int32_t>() == 13);
    CHECK(result.GetValue(0, 1).GetValue<int32_t>() == 30);
    CHECK(result.GetValue(0, 2).GetValue<int32_t>() == 7);
}

TEST_CASE("E2E - SELECT with NULL") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1)");
    conn.Query("INSERT INTO t VALUES (NULL)");
    conn.Query("INSERT INTO t VALUES (3)");

    auto result = conn.Query("SELECT x FROM t WHERE x IS NOT NULL");
    CHECK(result.RowCount() == 2);
}

TEST_CASE("E2E - INSERT multiple rows") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    auto result = conn.Query("SELECT x FROM t");
    CHECK(result.RowCount() == 3);
}

TEST_CASE("E2E - DROP TABLE") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE tmp (x INTEGER)");
    conn.Query("DROP TABLE tmp");
    CHECK_THROWS(conn.Query("SELECT * FROM tmp"));
}

TEST_CASE("E2E - CREATE TABLE IF NOT EXISTS") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("CREATE TABLE IF NOT EXISTS t (x INTEGER)"); // should not throw
    CHECK_NOTHROW(conn.Query("SELECT * FROM t"));
}

TEST_CASE("E2E - SELECT with AND/OR") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER, y INTEGER)");
    conn.Query("INSERT INTO t VALUES (1, 10)");
    conn.Query("INSERT INTO t VALUES (2, 20)");
    conn.Query("INSERT INTO t VALUES (3, 30)");

    auto result = conn.Query("SELECT x FROM t WHERE x > 1 AND y < 30");
    CHECK(result.RowCount() == 1);
    CHECK(result.GetValue(0, 0).GetValue<int32_t>() == 2);
}

TEST_CASE("E2E - SELECT with alias") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (42)");

    auto result = conn.Query("SELECT x AS my_val FROM t");
    CHECK(result.column_names[0] == "my_val");
    CHECK(result.GetValue(0, 0).GetValue<int32_t>() == 42);
}

TEST_CASE("E2E - QueryResult::ToString") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR)");
    conn.Query("INSERT INTO t VALUES (1, 'Alice')");

    auto result = conn.Query("SELECT id, name FROM t");
    auto str = result.ToString();
    CHECK(str.find("id") != std::string::npos);
    CHECK(str.find("Alice") != std::string::npos);
}

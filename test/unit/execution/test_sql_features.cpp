#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

// ============================================================================
// LIKE
// ============================================================================

TEST_CASE("SQL - LIKE pattern matching") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (name VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('Alice'), ('Bob'), ('Charlie'), ('Alex')");

    auto r = conn.Query("SELECT name FROM t WHERE name LIKE 'Al%'");
    CHECK(r.RowCount() == 2); // Alice, Alex

    r = conn.Query("SELECT name FROM t WHERE name LIKE '%li%'");
    CHECK(r.RowCount() == 2); // Alice, Charlie

    r = conn.Query("SELECT name FROM t WHERE name LIKE '___'");
    CHECK(r.RowCount() == 1); // Bob (3 chars)
}

// ============================================================================
// IN list
// ============================================================================

TEST_CASE("SQL - IN list") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3), (4), (5)");

    auto r = conn.Query("SELECT x FROM t WHERE x IN (2, 4)");
    CHECK(r.RowCount() == 2);
}

// ============================================================================
// CASE WHEN
// ============================================================================

TEST_CASE("SQL - CASE WHEN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (score INTEGER)");
    conn.Query("INSERT INTO t VALUES (95), (75), (50)");

    auto r = conn.Query(
        "SELECT CASE WHEN score >= 90 THEN 'A' "
        "WHEN score >= 70 THEN 'B' "
        "ELSE 'C' END FROM t");
    CHECK(r.RowCount() == 3);
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "A");
    CHECK(r.GetValue(1, 0).GetValue<std::string>() == "B");
    CHECK(r.GetValue(2, 0).GetValue<std::string>() == "C");
}

// ============================================================================
// CAST
// ============================================================================

TEST_CASE("SQL - CAST") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (42)");

    auto r = conn.Query("SELECT CAST(x AS VARCHAR) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "42");

    r = conn.Query("SELECT CAST(x AS DOUBLE) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(42.0));
}

// ============================================================================
// String functions
// ============================================================================

TEST_CASE("SQL - LENGTH function") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('hello'), (''), ('world!')");

    auto r = conn.Query("SELECT LENGTH(s) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 5);
    CHECK(r.GetValue(1, 0).GetValue<int32_t>() == 0);
    CHECK(r.GetValue(2, 0).GetValue<int32_t>() == 6);
}

TEST_CASE("SQL - UPPER and LOWER") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('Hello World')");

    auto r = conn.Query("SELECT UPPER(s) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "HELLO WORLD");

    r = conn.Query("SELECT LOWER(s) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "hello world");
}

TEST_CASE("SQL - CONCAT") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (first_name VARCHAR, last_name VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('John', 'Doe')");

    auto r = conn.Query("SELECT CONCAT(first_name, ' ', last_name) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "John Doe");
}

TEST_CASE("SQL - SUBSTRING") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('Hello World')");

    auto r = conn.Query("SELECT SUBSTRING(s, 1, 5) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "Hello");

    r = conn.Query("SELECT SUBSTRING(s, 7, 5) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "World");
}

TEST_CASE("SQL - REPLACE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('Hello World')");

    auto r = conn.Query("SELECT REPLACE(s, 'World', 'SlothDB') FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "Hello SlothDB");
}

// ============================================================================
// Math functions
// ============================================================================

TEST_CASE("SQL - ABS") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (-5), (3), (0)");

    auto r = conn.Query("SELECT ABS(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 5);
    CHECK(r.GetValue(1, 0).GetValue<int32_t>() == 3);
    CHECK(r.GetValue(2, 0).GetValue<int32_t>() == 0);
}

TEST_CASE("SQL - CEIL, FLOOR, ROUND") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x DOUBLE)");
    conn.Query("INSERT INTO t VALUES (3.7)");

    auto r = conn.Query("SELECT CEIL(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(4.0));

    r = conn.Query("SELECT FLOOR(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(3.0));

    r = conn.Query("SELECT ROUND(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(4.0));
}

TEST_CASE("SQL - SQRT") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x DOUBLE)");
    conn.Query("INSERT INTO t VALUES (16.0)");

    auto r = conn.Query("SELECT SQRT(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(4.0));
}

// ============================================================================
// Null handling functions
// ============================================================================

TEST_CASE("SQL - COALESCE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (a INTEGER, b INTEGER)");
    conn.Query("INSERT INTO t VALUES (NULL, 10)");
    conn.Query("INSERT INTO t VALUES (5, 20)");

    auto r = conn.Query("SELECT COALESCE(a, b) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 10);
    CHECK(r.GetValue(1, 0).GetValue<int32_t>() == 5);
}

TEST_CASE("SQL - NULLIF") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (0), (3)");

    auto r = conn.Query("SELECT NULLIF(x, 0) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 1);
    CHECK(r.GetValue(1, 0).IsNull()); // 0 = 0, so NULL
    CHECK(r.GetValue(2, 0).GetValue<int32_t>() == 3);
}

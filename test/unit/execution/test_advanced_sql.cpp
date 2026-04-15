#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

// ============================================================================
// DISTINCT (already tested, verify still works)
// ============================================================================

TEST_CASE("Advanced - SELECT DISTINCT") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (1), (2), (2), (3)");
    auto r = conn.Query("SELECT DISTINCT x FROM t");
    CHECK(r.RowCount() == 3);
}

// ============================================================================
// New scalar string functions
// ============================================================================

TEST_CASE("Advanced - POSITION/STRPOS") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('hello world')");

    auto r = conn.Query("SELECT POSITION(s, 'world') FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 7);
}

TEST_CASE("Advanced - LEFT and RIGHT") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('hello')");

    auto r = conn.Query("SELECT LEFT(s, 3) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "hel");

    r = conn.Query("SELECT RIGHT(s, 3) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "llo");
}

TEST_CASE("Advanced - REVERSE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('abc')");

    auto r = conn.Query("SELECT REVERSE(s) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "cba");
}

TEST_CASE("Advanced - REPEAT") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('ha')");

    auto r = conn.Query("SELECT REPEAT(s, 3) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "hahaha");
}

TEST_CASE("Advanced - STARTS_WITH and ENDS_WITH") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('hello world')");

    auto r = conn.Query("SELECT STARTS_WITH(s, 'hello') FROM t");
    CHECK(r.GetValue(0, 0).GetValue<bool>() == true);

    r = conn.Query("SELECT ENDS_WITH(s, 'world') FROM t");
    CHECK(r.GetValue(0, 0).GetValue<bool>() == true);

    r = conn.Query("SELECT CONTAINS(s, 'lo wo') FROM t");
    CHECK(r.GetValue(0, 0).GetValue<bool>() == true);
}

TEST_CASE("Advanced - LPAD and RPAD") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('hi')");

    auto r = conn.Query("SELECT LPAD(s, 5, '0') FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "000hi");

    r = conn.Query("SELECT RPAD(s, 5, '.') FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "hi...");
}

// ============================================================================
// New math functions
// ============================================================================

TEST_CASE("Advanced - LOG, EXP, SIGN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x DOUBLE)");
    conn.Query("INSERT INTO t VALUES (100.0)");

    auto r = conn.Query("SELECT LOG10(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(2.0));

    r = conn.Query("SELECT LN(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(4.605).epsilon(0.01));

    r = conn.Query("SELECT SIGN(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 1);
}

TEST_CASE("Advanced - LEAST and GREATEST") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (a INTEGER, b INTEGER, c INTEGER)");
    conn.Query("INSERT INTO t VALUES (10, 5, 20)");

    auto r = conn.Query("SELECT LEAST(a, b, c) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 5);

    r = conn.Query("SELECT GREATEST(a, b, c) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 20);
}

TEST_CASE("Advanced - PI") {
    Database db;
    Connection conn(db);
    auto r = conn.Query("SELECT PI()");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(3.14159).epsilon(0.001));
}

// ============================================================================
// New aggregate functions
// ============================================================================

TEST_CASE("Advanced - STDDEV") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (2), (4), (4), (4), (5), (5), (7), (9)");

    auto r = conn.Query("SELECT STDDEV(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(2.0).epsilon(0.1));
}

TEST_CASE("Advanced - VARIANCE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (2), (4), (4), (4), (5), (5), (7), (9)");

    auto r = conn.Query("SELECT VARIANCE(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(4.0).epsilon(0.5));
}

TEST_CASE("Advanced - MEDIAN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (3), (5), (7), (9)");

    auto r = conn.Query("SELECT MEDIAN(x) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<double>() == doctest::Approx(5.0));
}

TEST_CASE("Advanced - STRING_AGG") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (name VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('Alice'), ('Bob'), ('Charlie')");

    auto r = conn.Query("SELECT STRING_AGG(name, ', ') FROM t");
    auto val = r.GetValue(0, 0).GetValue<std::string>();
    CHECK(val.find("Alice") != std::string::npos);
    CHECK(val.find("Bob") != std::string::npos);
    CHECK(val.find("Charlie") != std::string::npos);
}

// ============================================================================
// Date functions
// ============================================================================

TEST_CASE("Advanced - STRFTIME") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE ts (t BIGINT)");
    conn.Query("INSERT INTO ts VALUES (1718451000000000)"); // 2024-06-15

    auto r = conn.Query("SELECT STRFTIME(t) FROM ts");
    auto val = r.GetValue(0, 0).GetValue<std::string>();
    CHECK(val.find("2024") != std::string::npos);
    CHECK(val.find("06") != std::string::npos);
}

TEST_CASE("Advanced - DATE_ADD") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE ts (t BIGINT)");
    conn.Query("INSERT INTO ts VALUES (1718451000000000)");

    auto r = conn.Query("SELECT DATE_ADD('DAY', 1, t) FROM ts");
    auto added = r.GetValue(0, 0).GetValue<int64_t>();
    CHECK(added == 1718451000000000LL + 86400LL * 1000000);
}

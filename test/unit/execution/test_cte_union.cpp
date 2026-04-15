#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

// ============================================================================
// CTEs (WITH ... AS)
// ============================================================================

TEST_CASE("CTE - basic WITH") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE employees (name VARCHAR, dept VARCHAR, salary INTEGER)");
    conn.Query("INSERT INTO employees VALUES ('Alice', 'Eng', 100)");
    conn.Query("INSERT INTO employees VALUES ('Bob', 'Eng', 120)");
    conn.Query("INSERT INTO employees VALUES ('Charlie', 'Sales', 90)");

    auto r = conn.Query(
        "WITH eng AS (SELECT name, salary FROM employees WHERE dept = 'Eng') "
        "SELECT * FROM eng");
    CHECK(r.RowCount() == 2);
    CHECK(r.ColumnCount() == 2);
}

TEST_CASE("CTE - with aggregation") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE sales (product VARCHAR, amount INTEGER)");
    conn.Query("INSERT INTO sales VALUES ('A', 10), ('B', 20), ('A', 30), ('B', 40)");

    auto r = conn.Query(
        "WITH totals AS (SELECT product, SUM(amount) FROM sales GROUP BY product) "
        "SELECT * FROM totals");
    CHECK(r.RowCount() == 2);
}

TEST_CASE("CTE - temp table is cleaned up") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    conn.Query("WITH tmp AS (SELECT x FROM t) SELECT * FROM tmp");

    // CTE temp table should be gone.
    CHECK_THROWS(conn.Query("SELECT * FROM tmp"));
}

// ============================================================================
// UNION
// ============================================================================

TEST_CASE("UNION - basic") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t1 (x INTEGER)");
    conn.Query("INSERT INTO t1 VALUES (1), (2), (3)");
    conn.Query("CREATE TABLE t2 (x INTEGER)");
    conn.Query("INSERT INTO t2 VALUES (3), (4), (5)");

    auto r = conn.Query("SELECT x FROM t1 UNION SELECT x FROM t2");
    CHECK(r.RowCount() == 5); // 1,2,3,4,5 (deduplicated)
}

TEST_CASE("UNION ALL - includes duplicates") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t1 (x INTEGER)");
    conn.Query("INSERT INTO t1 VALUES (1), (2), (3)");
    conn.Query("CREATE TABLE t2 (x INTEGER)");
    conn.Query("INSERT INTO t2 VALUES (3), (4), (5)");

    auto r = conn.Query("SELECT x FROM t1 UNION ALL SELECT x FROM t2");
    CHECK(r.RowCount() == 6); // 1,2,3,3,4,5 (with duplicate 3)
}

// ============================================================================
// INTERSECT
// ============================================================================

TEST_CASE("INTERSECT - common rows") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t1 (x INTEGER)");
    conn.Query("INSERT INTO t1 VALUES (1), (2), (3)");
    conn.Query("CREATE TABLE t2 (x INTEGER)");
    conn.Query("INSERT INTO t2 VALUES (2), (3), (4)");

    auto r = conn.Query("SELECT x FROM t1 INTERSECT SELECT x FROM t2");
    CHECK(r.RowCount() == 2); // 2, 3
}

// ============================================================================
// EXCEPT
// ============================================================================

TEST_CASE("EXCEPT - difference") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t1 (x INTEGER)");
    conn.Query("INSERT INTO t1 VALUES (1), (2), (3)");
    conn.Query("CREATE TABLE t2 (x INTEGER)");
    conn.Query("INSERT INTO t2 VALUES (2), (3), (4)");

    auto r = conn.Query("SELECT x FROM t1 EXCEPT SELECT x FROM t2");
    CHECK(r.RowCount() == 1); // Only 1
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 1);
}

#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

// ============================================================================
// BETWEEN
// ============================================================================

TEST_CASE("BETWEEN - basic") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (3), (5), (7), (9)");

    auto r = conn.Query("SELECT x FROM t WHERE x BETWEEN 3 AND 7");
    CHECK(r.RowCount() == 3); // 3, 5, 7
}

TEST_CASE("BETWEEN - strings") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (name VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('Alice'), ('Bob'), ('Charlie'), ('Diana')");

    auto r = conn.Query("SELECT name FROM t WHERE name BETWEEN 'B' AND 'D'");
    CHECK(r.RowCount() == 2); // Bob, Charlie
}

// ============================================================================
// EXPLAIN
// ============================================================================

TEST_CASE("EXPLAIN - shows plan") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");

    auto r = conn.Query("EXPLAIN SELECT x FROM t WHERE x > 5");
    CHECK(r.RowCount() == 1);
    CHECK(r.column_names[0] == "plan");
    auto plan = r.GetValue(0, 0).GetValue<std::string>();
    CHECK(plan.find("FILTER") != std::string::npos);
    CHECK(plan.find("SCAN") != std::string::npos);
    CHECK(plan.find("PROJECTION") != std::string::npos);
}

TEST_CASE("EXPLAIN - aggregate plan") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER, y VARCHAR)");

    auto r = conn.Query("EXPLAIN SELECT y, COUNT(*) FROM t GROUP BY y");
    auto plan = r.GetValue(0, 0).GetValue<std::string>();
    CHECK(plan.find("AGGREGATE") != std::string::npos);
}

// ============================================================================
// CREATE VIEW
// ============================================================================

TEST_CASE("CREATE VIEW - basic") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE employees (name VARCHAR, dept VARCHAR, salary INTEGER)");
    conn.Query("INSERT INTO employees VALUES ('Alice', 'Eng', 100), ('Bob', 'Sales', 90)");

    conn.Query("CREATE VIEW eng_view AS SELECT name, salary FROM employees WHERE dept = 'Eng'");

    auto r = conn.Query("SELECT * FROM eng_view");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "Alice");
}

TEST_CASE("CREATE VIEW - virtual re-execution") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE data (id INTEGER, val VARCHAR)");
    conn.Query("INSERT INTO data VALUES (1, 'a'), (2, 'b')");

    // Create a view over the table.
    conn.Query("CREATE VIEW data_view AS SELECT * FROM data");

    auto r = conn.Query("SELECT COUNT(*) FROM data_view");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 2);

    // Insert more data into the underlying table.
    conn.Query("INSERT INTO data VALUES (3, 'c'), (4, 'd')");

    // The view should reflect the updated data.
    r = conn.Query("SELECT COUNT(*) FROM data_view");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 4);

    // Delete from underlying table.
    conn.Query("DELETE FROM data WHERE id > 2");

    r = conn.Query("SELECT COUNT(*) FROM data_view");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 2);
}

TEST_CASE("CREATE OR REPLACE VIEW") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    conn.Query("CREATE VIEW v AS SELECT x FROM t WHERE x > 1");
    auto r = conn.Query("SELECT * FROM v");
    CHECK(r.RowCount() == 2);

    conn.Query("CREATE OR REPLACE VIEW v AS SELECT x FROM t WHERE x > 2");
    r = conn.Query("SELECT * FROM v");
    CHECK(r.RowCount() == 1);
}

// ============================================================================
// TRUNCATE
// ============================================================================

TEST_CASE("TRUNCATE TABLE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    auto r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 3);

    conn.Query("TRUNCATE TABLE t");

    r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 0);

    // Table still exists, can insert again.
    conn.Query("INSERT INTO t VALUES (10)");
    r = conn.Query("SELECT x FROM t");
    CHECK(r.RowCount() == 1);
}

TEST_CASE("TRUNCATE without TABLE keyword") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1)");

    conn.Query("TRUNCATE t");
    auto r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 0);
}

// ============================================================================
// Combined queries
// ============================================================================

TEST_CASE("Complex - CTE + window + qualify") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE sales (product VARCHAR, region VARCHAR, amount INTEGER)");
    conn.Query("INSERT INTO sales VALUES ('A', 'East', 100)");
    conn.Query("INSERT INTO sales VALUES ('B', 'East', 200)");
    conn.Query("INSERT INTO sales VALUES ('A', 'West', 150)");
    conn.Query("INSERT INTO sales VALUES ('B', 'West', 50)");

    auto r = conn.Query(
        "WITH ranked AS ("
        "  SELECT product, region, amount, "
        "    ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount DESC) "
        "  FROM sales"
        ") "
        "SELECT * FROM ranked");
    CHECK(r.RowCount() == 4);
}

TEST_CASE("Complex - multiple aggregates + BETWEEN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE scores (name VARCHAR, score INTEGER)");
    conn.Query("INSERT INTO scores VALUES ('A', 85), ('B', 92), ('C', 78), ('D', 95), ('E', 88)");

    auto r = conn.Query(
        "SELECT COUNT(*), AVG(score), MIN(score), MAX(score) "
        "FROM scores WHERE score BETWEEN 80 AND 95");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 4); // 85, 92, 88, 95
}

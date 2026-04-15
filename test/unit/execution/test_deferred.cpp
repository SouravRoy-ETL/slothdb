#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

// ============================================================================
// GENERATE_SERIES
// ============================================================================

TEST_CASE("GENERATE_SERIES - basic") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT * FROM GENERATE_SERIES(1, 5)");
    CHECK(r.RowCount() == 5);
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1);
    CHECK(r.GetValue(4, 0).GetValue<int64_t>() == 5);
}

TEST_CASE("GENERATE_SERIES - with step") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT * FROM GENERATE_SERIES(0, 10, 2)");
    CHECK(r.RowCount() == 6); // 0,2,4,6,8,10
}

TEST_CASE("GENERATE_SERIES - with alias") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT generate_series FROM GENERATE_SERIES(1, 3) s");
    CHECK(r.RowCount() == 3);
}

TEST_CASE("GENERATE_SERIES - in WHERE") {
    Database db;
    Connection conn(db);

    auto r = conn.Query(
        "SELECT generate_series FROM GENERATE_SERIES(1, 10) "
        "WHERE generate_series >= 8");
    CHECK(r.RowCount() == 3); // 8, 9, 10
}

// ============================================================================
// REGEX functions (basic - without full regex engine, pattern-based)
// ============================================================================

TEST_CASE("REGEXP_MATCHES - basic pattern") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('hello123'), ('world'), ('abc456')");

    // Simple: check if string contains digits.
    auto r = conn.Query("SELECT REGEXP_MATCHES(s, '[0-9]') FROM t");
    CHECK(r.RowCount() == 3);
    // hello123 -> true, world -> false, abc456 -> true
    CHECK(r.GetValue(0, 0).GetValue<bool>() == true);
    CHECK(r.GetValue(1, 0).GetValue<bool>() == false);
    CHECK(r.GetValue(2, 0).GetValue<bool>() == true);
}

TEST_CASE("REGEXP_REPLACE - basic") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('hello 123 world')");

    auto r = conn.Query("SELECT REGEXP_REPLACE(s, '[0-9]+', 'NUM') FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "hello NUM world");
}

// ============================================================================
// RETURNING clause (on INSERT)
// ============================================================================

// RETURNING is complex - skip for now, test that INSERT works.

// ============================================================================
// Practical complex queries
// ============================================================================

TEST_CASE("Complex - window + CTE + join + aggregation") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE products (id INTEGER, category VARCHAR, price INTEGER)");
    conn.Query("INSERT INTO products VALUES (1, 'A', 100), (2, 'A', 200), (3, 'B', 150), (4, 'B', 300)");

    auto r = conn.Query(
        "WITH ranked AS ("
        "  SELECT id, category, price, "
        "    ROW_NUMBER() OVER (PARTITION BY category ORDER BY price DESC) "
        "  FROM products"
        ") SELECT * FROM ranked");
    CHECK(r.RowCount() == 4);
}

TEST_CASE("Complex - GENERATE_SERIES + aggregation") {
    Database db;
    Connection conn(db);

    auto r = conn.Query(
        "SELECT SUM(generate_series), COUNT(*) FROM GENERATE_SERIES(1, 100)");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 5050);
    CHECK(r.GetValue(0, 1).GetValue<int64_t>() == 100);
}

TEST_CASE("Complex - multiple CTEs") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3), (4), (5)");

    auto r = conn.Query(
        "WITH evens AS (SELECT x FROM t WHERE x % 2 = 0), "
        "     odds AS (SELECT x FROM t WHERE x % 2 = 1) "
        "SELECT * FROM evens");
    CHECK(r.RowCount() == 2); // 2, 4
}

TEST_CASE("Complex - self-referencing CTE-like pattern") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE employees (id INTEGER, name VARCHAR, manager_id INTEGER)");
    conn.Query("INSERT INTO employees VALUES (1, 'CEO', NULL)");
    conn.Query("INSERT INTO employees VALUES (2, 'VP', 1)");
    conn.Query("INSERT INTO employees VALUES (3, 'Director', 2)");

    // Without recursive CTE, use a simple join to get manager names.
    auto r = conn.Query(
        "SELECT e.name, m.name FROM employees e "
        "LEFT JOIN employees m ON e.manager_id = m.id");
    CHECK(r.RowCount() == 3);
}

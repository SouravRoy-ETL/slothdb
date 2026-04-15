#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

// ============================================================================
// ALTER TABLE
// ============================================================================

TEST_CASE("ALTER TABLE ADD COLUMN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR)");
    conn.Query("INSERT INTO t VALUES (1, 'Alice'), (2, 'Bob')");

    conn.Query("ALTER TABLE t ADD COLUMN score INTEGER");

    auto r = conn.Query("SELECT * FROM t");
    CHECK(r.ColumnCount() == 3);
    CHECK(r.RowCount() == 2);
    // New column should be NULL.
    CHECK(r.GetValue(0, 2).IsNull());
    CHECK(r.GetValue(1, 2).IsNull());
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 1); // existing data preserved
}

TEST_CASE("ALTER TABLE DROP COLUMN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (a INTEGER, b VARCHAR, c DOUBLE)");
    conn.Query("INSERT INTO t VALUES (1, 'x', 3.14)");

    conn.Query("ALTER TABLE t DROP COLUMN b");

    auto r = conn.Query("SELECT * FROM t");
    CHECK(r.ColumnCount() == 2);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 1);
    CHECK(r.GetValue(0, 1).GetValue<double>() == doctest::Approx(3.14));
}

TEST_CASE("ALTER TABLE RENAME COLUMN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (old_name INTEGER)");
    conn.Query("INSERT INTO t VALUES (42)");

    conn.Query("ALTER TABLE t RENAME COLUMN old_name TO new_name");

    auto r = conn.Query("SELECT new_name FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 42);
}

// ============================================================================
// String || concatenation
// ============================================================================

TEST_CASE("|| string concat") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (a VARCHAR, b VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('hello', ' world')");

    auto r = conn.Query("SELECT a || b FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "hello world");
}

TEST_CASE("|| multi concat") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (a VARCHAR, b VARCHAR, c VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('a', 'b', 'c')");

    auto r = conn.Query("SELECT a || b || c FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "abc");
}

// ============================================================================
// Mixed complex queries
// ============================================================================

TEST_CASE("Complex - INSERT SELECT with aggregation") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE sales (product VARCHAR, amount INTEGER)");
    conn.Query("INSERT INTO sales VALUES ('A', 10), ('B', 20), ('A', 30)");
    conn.Query("CREATE TABLE summary (product VARCHAR, total INTEGER)");

    conn.Query("INSERT INTO summary SELECT product, SUM(amount) FROM sales GROUP BY product");

    auto r = conn.Query("SELECT * FROM summary");
    CHECK(r.RowCount() == 2);
}

TEST_CASE("Complex - UPDATE with expression") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (10), (20), (30)");

    conn.Query("UPDATE t SET x = x * 2 WHERE x > 15");

    auto r = conn.Query("SELECT x FROM t");
    // 10 unchanged, 20->40, 30->60
    bool found10 = false, found40 = false, found60 = false;
    for (idx_t i = 0; i < r.RowCount(); i++) {
        auto v = r.GetValue(i, 0).GetValue<int32_t>();
        if (v == 10) found10 = true;
        if (v == 40) found40 = true;
        if (v == 60) found60 = true;
    }
    CHECK(found10);
    CHECK(found40);
    CHECK(found60);
}

TEST_CASE("Complex - CTE with JOIN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE orders (id INTEGER, customer_id INTEGER, amount INTEGER)");
    conn.Query("INSERT INTO orders VALUES (1, 1, 100), (2, 2, 200), (3, 1, 150)");
    conn.Query("CREATE TABLE customers (id INTEGER, name VARCHAR)");
    conn.Query("INSERT INTO customers VALUES (1, 'Alice'), (2, 'Bob')");

    auto r = conn.Query(
        "WITH order_totals AS ("
        "  SELECT customer_id, SUM(amount) FROM orders GROUP BY customer_id"
        ") "
        "SELECT c.name, o.customer_id FROM customers c "
        "INNER JOIN order_totals o ON c.id = o.customer_id");
    CHECK(r.RowCount() == 2);
}

TEST_CASE("Complex - nested functions") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (s VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('  Hello World  ')");

    auto r = conn.Query("SELECT UPPER(TRIM(s)) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "HELLO WORLD");
}

TEST_CASE("Complex - CASE with aggregation") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE scores (name VARCHAR, score INTEGER)");
    conn.Query("INSERT INTO scores VALUES ('A', 95), ('B', 75), ('C', 85), ('D', 65)");

    auto r = conn.Query(
        "SELECT "
        "  COUNT(CASE WHEN score >= 90 THEN 1 END), "
        "  COUNT(CASE WHEN score >= 70 AND score < 90 THEN 1 END), "
        "  COUNT(CASE WHEN score < 70 THEN 1 END) "
        "FROM scores");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1); // A
    CHECK(r.GetValue(0, 1).GetValue<int64_t>() == 2); // B, C
    CHECK(r.GetValue(0, 2).GetValue<int64_t>() == 1); // D
}

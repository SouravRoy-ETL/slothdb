#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

TEST_CASE("EXISTS - basic true (non-correlated)") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t1 (x INTEGER)");
    conn.Query("INSERT INTO t1 VALUES (1), (2), (3)");
    conn.Query("CREATE TABLE t2 (y INTEGER)");
    conn.Query("INSERT INTO t2 VALUES (2), (3), (4)");

    // Non-correlated EXISTS: checks if subquery returns any rows.
    auto r = conn.Query(
        "SELECT x FROM t1 WHERE EXISTS (SELECT 1 FROM t2)");
    CHECK(r.RowCount() == 3); // t2 is non-empty -> all rows returned
}

TEST_CASE("EXISTS - with filter in subquery") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t1 (x INTEGER)");
    conn.Query("INSERT INTO t1 VALUES (1), (2), (3)");
    conn.Query("CREATE TABLE t2 (y INTEGER)");
    conn.Query("INSERT INTO t2 VALUES (10), (20)");

    auto r = conn.Query(
        "SELECT x FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE y > 100)");
    CHECK(r.RowCount() == 0); // No rows in t2 with y > 100 -> EXISTS is false
}

TEST_CASE("EXISTS - empty subquery returns nothing") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");
    conn.Query("CREATE TABLE empty_t (y INTEGER)");

    auto r = conn.Query(
        "SELECT x FROM t WHERE EXISTS (SELECT 1 FROM empty_t)");
    CHECK(r.RowCount() == 0); // subquery is empty -> EXISTS is false
}

TEST_CASE("NOT EXISTS") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");
    conn.Query("CREATE TABLE empty_t (y INTEGER)");

    auto r = conn.Query(
        "SELECT x FROM t WHERE NOT EXISTS (SELECT 1 FROM empty_t)");
    CHECK(r.RowCount() == 3); // subquery is empty -> NOT EXISTS is true
}

TEST_CASE("IN subquery") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE orders (id INTEGER, product VARCHAR)");
    conn.Query("INSERT INTO orders VALUES (1, 'A'), (2, 'B'), (3, 'C')");
    conn.Query("CREATE TABLE products (name VARCHAR, active INTEGER)");
    conn.Query("INSERT INTO products VALUES ('A', 1), ('C', 1)");

    auto r = conn.Query(
        "SELECT id, product FROM orders WHERE product IN (SELECT name FROM products)");
    CHECK(r.RowCount() == 2); // A and C
}

TEST_CASE("NOT IN subquery") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t1 (x INTEGER)");
    conn.Query("INSERT INTO t1 VALUES (1), (2), (3), (4), (5)");
    conn.Query("CREATE TABLE t2 (y INTEGER)");
    conn.Query("INSERT INTO t2 VALUES (2), (4)");

    auto r = conn.Query(
        "SELECT x FROM t1 WHERE NOT x IN (SELECT y FROM t2)");
    CHECK(r.RowCount() == 3); // 1, 3, 5
}

TEST_CASE("EXISTS with real-world pattern - anti-join") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE customers (id INTEGER, name VARCHAR)");
    conn.Query("INSERT INTO customers VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie')");
    conn.Query("CREATE TABLE orders (customer_id INTEGER, amount INTEGER)");
    conn.Query("INSERT INTO orders VALUES (1, 100), (1, 200), (3, 50)");

    // Customers who have at least one order.
    auto r = conn.Query(
        "SELECT name FROM customers WHERE EXISTS ("
        "  SELECT 1 FROM orders WHERE orders.customer_id > 0"
        ")");
    // Non-correlated: orders table is non-empty -> all customers returned.
    CHECK(r.RowCount() == 3);
}

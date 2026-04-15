#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

static void setup_join_tables(Connection &conn) {
    conn.Query("CREATE TABLE orders (id INTEGER, customer_id INTEGER, amount DOUBLE)");
    conn.Query("INSERT INTO orders VALUES (1, 1, 100.0)");
    conn.Query("INSERT INTO orders VALUES (2, 2, 200.0)");
    conn.Query("INSERT INTO orders VALUES (3, 1, 150.0)");
    conn.Query("INSERT INTO orders VALUES (4, 3, 300.0)");

    conn.Query("CREATE TABLE customers (id INTEGER, name VARCHAR)");
    conn.Query("INSERT INTO customers VALUES (1, 'Alice')");
    conn.Query("INSERT INTO customers VALUES (2, 'Bob')");
    conn.Query("INSERT INTO customers VALUES (4, 'Diana')"); // no orders for Diana
}

TEST_CASE("JOIN - INNER JOIN") {
    Database db;
    Connection conn(db);
    setup_join_tables(conn);

    auto result = conn.Query(
        "SELECT orders.id, customers.name, orders.amount "
        "FROM orders INNER JOIN customers ON orders.customer_id = customers.id");

    // Orders 1,2,3 match (customer_id 1,2,1). Order 4 (customer_id 3) has no match.
    CHECK(result.RowCount() == 3);
}

TEST_CASE("JOIN - LEFT JOIN") {
    Database db;
    Connection conn(db);
    setup_join_tables(conn);

    auto result = conn.Query(
        "SELECT orders.id, customers.name "
        "FROM orders LEFT JOIN customers ON orders.customer_id = customers.id");

    // All 4 orders appear. Order 4 has NULL for customer name.
    CHECK(result.RowCount() == 4);

    // Find the row with order_id 4.
    bool found_null = false;
    for (idx_t i = 0; i < result.RowCount(); i++) {
        if (result.GetValue(i, 0).GetValue<int32_t>() == 4) {
            CHECK(result.GetValue(i, 1).IsNull());
            found_null = true;
        }
    }
    CHECK(found_null);
}

TEST_CASE("JOIN - RIGHT JOIN") {
    Database db;
    Connection conn(db);
    setup_join_tables(conn);

    auto result = conn.Query(
        "SELECT orders.id, customers.name "
        "FROM orders RIGHT JOIN customers ON orders.customer_id = customers.id");

    // All customers appear. Diana (id=4) has NULL for order id.
    CHECK(result.RowCount() == 4); // 3 matched + 1 unmatched (Diana)

    bool found_diana = false;
    for (idx_t i = 0; i < result.RowCount(); i++) {
        if (!result.GetValue(i, 1).IsNull() &&
            result.GetValue(i, 1).GetValue<std::string>() == "Diana") {
            CHECK(result.GetValue(i, 0).IsNull());
            found_diana = true;
        }
    }
    CHECK(found_diana);
}

TEST_CASE("JOIN - CROSS JOIN") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t1 (x INTEGER)");
    conn.Query("INSERT INTO t1 VALUES (1), (2)");
    conn.Query("CREATE TABLE t2 (y INTEGER)");
    conn.Query("INSERT INTO t2 VALUES (10), (20), (30)");

    auto result = conn.Query("SELECT * FROM t1 CROSS JOIN t2");
    CHECK(result.RowCount() == 6); // 2 * 3
}

TEST_CASE("JOIN - with aliases") {
    Database db;
    Connection conn(db);
    setup_join_tables(conn);

    auto result = conn.Query(
        "SELECT o.id, c.name FROM orders o INNER JOIN customers c ON o.customer_id = c.id");
    CHECK(result.RowCount() == 3);
}

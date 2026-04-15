#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

static void setup_window_data(Connection &conn) {
    conn.Query("CREATE TABLE sales (name VARCHAR, dept VARCHAR, amount INTEGER)");
    conn.Query("INSERT INTO sales VALUES ('Alice', 'Engineering', 100)");
    conn.Query("INSERT INTO sales VALUES ('Bob', 'Engineering', 200)");
    conn.Query("INSERT INTO sales VALUES ('Charlie', 'Sales', 150)");
    conn.Query("INSERT INTO sales VALUES ('Diana', 'Sales', 300)");
    conn.Query("INSERT INTO sales VALUES ('Eve', 'Sales', 250)");
}

// ============================================================================
// DISTINCT
// ============================================================================

TEST_CASE("DISTINCT - basic") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (2), (3), (1), (3)");

    auto r = conn.Query("SELECT DISTINCT x FROM t");
    CHECK(r.RowCount() == 3);
}

TEST_CASE("DISTINCT - multi-column") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (a INTEGER, b VARCHAR)");
    conn.Query("INSERT INTO t VALUES (1, 'x'), (1, 'y'), (1, 'x'), (2, 'x')");

    auto r = conn.Query("SELECT DISTINCT a, b FROM t");
    CHECK(r.RowCount() == 3); // (1,x), (1,y), (2,x)
}

// ============================================================================
// Window Functions
// ============================================================================

TEST_CASE("Window - ROW_NUMBER()") {
    Database db;
    Connection conn(db);
    setup_window_data(conn);

    auto r = conn.Query(
        "SELECT name, ROW_NUMBER() OVER (ORDER BY amount) FROM sales");
    CHECK(r.RowCount() == 5);

    // Check that row numbers are 1-5.
    bool found[5] = {};
    for (idx_t i = 0; i < r.RowCount(); i++) {
        auto rn = r.GetValue(i, 1).GetValue<int64_t>();
        CHECK(rn >= 1);
        CHECK(rn <= 5);
        found[rn - 1] = true;
    }
    for (int i = 0; i < 5; i++) CHECK(found[i]);
}

TEST_CASE("Window - ROW_NUMBER() PARTITION BY") {
    Database db;
    Connection conn(db);
    setup_window_data(conn);

    auto r = conn.Query(
        "SELECT name, dept, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY amount) FROM sales");
    CHECK(r.RowCount() == 5);

    // Engineering has 2 rows (row_number 1, 2).
    // Sales has 3 rows (row_number 1, 2, 3).
    for (idx_t i = 0; i < r.RowCount(); i++) {
        auto dept = r.GetValue(i, 1).GetValue<std::string>();
        auto rn = r.GetValue(i, 2).GetValue<int64_t>();
        if (dept == "Engineering") CHECK(rn <= 2);
        if (dept == "Sales") CHECK(rn <= 3);
    }
}

TEST_CASE("Window - RANK()") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE scores (name VARCHAR, score INTEGER)");
    conn.Query("INSERT INTO scores VALUES ('A', 100), ('B', 90), ('C', 90), ('D', 80)");

    auto r = conn.Query(
        "SELECT name, RANK() OVER (ORDER BY score DESC) FROM scores");
    CHECK(r.RowCount() == 4);

    // A=1, B=2, C=2, D=4 (gap after tie).
    for (idx_t i = 0; i < r.RowCount(); i++) {
        auto name = r.GetValue(i, 0).GetValue<std::string>();
        auto rank = r.GetValue(i, 1).GetValue<int64_t>();
        if (name == "A") CHECK(rank == 1);
        if (name == "B") CHECK(rank == 2);
        if (name == "C") CHECK(rank == 2);
        if (name == "D") CHECK(rank == 4);
    }
}

TEST_CASE("Window - DENSE_RANK()") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE scores (name VARCHAR, score INTEGER)");
    conn.Query("INSERT INTO scores VALUES ('A', 100), ('B', 90), ('C', 90), ('D', 80)");

    auto r = conn.Query(
        "SELECT name, DENSE_RANK() OVER (ORDER BY score DESC) FROM scores");

    for (idx_t i = 0; i < r.RowCount(); i++) {
        auto name = r.GetValue(i, 0).GetValue<std::string>();
        auto rank = r.GetValue(i, 1).GetValue<int64_t>();
        if (name == "A") CHECK(rank == 1);
        if (name == "B") CHECK(rank == 2);
        if (name == "C") CHECK(rank == 2);
        if (name == "D") CHECK(rank == 3); // no gap!
    }
}

TEST_CASE("Window - LEAD and LAG") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (10), (20), (30), (40)");

    auto r = conn.Query(
        "SELECT x, LAG(x) OVER (ORDER BY x), LEAD(x) OVER (ORDER BY x) FROM t");
    CHECK(r.RowCount() == 4);

    // First row: LAG is NULL, value is 10, LEAD is 20.
    CHECK(r.GetValue(0, 1).IsNull()); // LAG of first
    CHECK(r.GetValue(3, 2).IsNull()); // LEAD of last
}

TEST_CASE("Window - NTILE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3), (4), (5), (6)");

    auto r = conn.Query(
        "SELECT x, NTILE(3) OVER (ORDER BY x) FROM t");
    CHECK(r.RowCount() == 6);

    // 6 rows, 3 buckets: bucket sizes 2,2,2.
    for (idx_t i = 0; i < r.RowCount(); i++) {
        auto bucket = r.GetValue(i, 1).GetValue<int64_t>();
        CHECK(bucket >= 1);
        CHECK(bucket <= 3);
    }
}

TEST_CASE("Window - SUM() OVER (PARTITION BY)") {
    Database db;
    Connection conn(db);
    setup_window_data(conn);

    auto r = conn.Query(
        "SELECT name, dept, SUM(amount) OVER (PARTITION BY dept) FROM sales");
    CHECK(r.RowCount() == 5);

    for (idx_t i = 0; i < r.RowCount(); i++) {
        auto dept = r.GetValue(i, 1).GetValue<std::string>();
        auto total = r.GetValue(i, 2).GetValue<int64_t>();
        if (dept == "Engineering") CHECK(total == 300); // 100 + 200
        if (dept == "Sales") CHECK(total == 700);       // 150 + 300 + 250
    }
}

// ============================================================================
// QUALIFY (Snowflake-style)
// ============================================================================

TEST_CASE("QUALIFY - filter on ROW_NUMBER") {
    Database db;
    Connection conn(db);
    setup_window_data(conn);

    // Get the top earner per department.
    auto r = conn.Query(
        "SELECT name, dept, amount, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY amount DESC) "
        "FROM sales "
        "QUALIFY ROW_NUMBER() OVER (PARTITION BY dept ORDER BY amount DESC) = 1");

    CHECK(r.RowCount() == 2); // One per department.

    for (idx_t i = 0; i < r.RowCount(); i++) {
        auto dept = r.GetValue(i, 1).GetValue<std::string>();
        auto name = r.GetValue(i, 0).GetValue<std::string>();
        if (dept == "Engineering") CHECK(name == "Bob");    // 200
        if (dept == "Sales") CHECK(name == "Diana");        // 300
    }
}

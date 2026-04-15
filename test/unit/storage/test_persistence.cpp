#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include <cstdio>

using namespace slothdb;

static const char *TEST_DB_PATH = "test_slothdb.db";

// Clean up test file.
static void cleanup() {
    std::remove(TEST_DB_PATH);
}

TEST_CASE("Persistence - save and load basic table") {
    cleanup();

    // Create DB, insert data, close (triggers save).
    {
        Database db(TEST_DB_PATH);
        Connection conn(db);
        conn.Query("CREATE TABLE users (id INTEGER, name VARCHAR, score DOUBLE)");
        conn.Query("INSERT INTO users VALUES (1, 'Alice', 95.5)");
        conn.Query("INSERT INTO users VALUES (2, 'Bob', 87.3)");
        conn.Query("INSERT INTO users VALUES (3, 'Charlie', 92.1)");
    } // Database destructor saves to file.

    // Reopen and verify data persists.
    {
        Database db(TEST_DB_PATH);
        Connection conn(db);

        auto r = conn.Query("SELECT * FROM users");
        CHECK(r.RowCount() == 3);
        CHECK(r.ColumnCount() == 3);

        // Verify values.
        bool found_alice = false, found_bob = false;
        for (idx_t i = 0; i < r.RowCount(); i++) {
            auto name = r.GetValue(i, 1).GetValue<std::string>();
            if (name == "Alice") {
                CHECK(r.GetValue(i, 0).GetValue<int32_t>() == 1);
                CHECK(r.GetValue(i, 2).GetValue<double>() == doctest::Approx(95.5));
                found_alice = true;
            }
            if (name == "Bob") {
                CHECK(r.GetValue(i, 0).GetValue<int32_t>() == 2);
                found_bob = true;
            }
        }
        CHECK(found_alice);
        CHECK(found_bob);
    }

    cleanup();
}

TEST_CASE("Persistence - multiple tables") {
    cleanup();

    {
        Database db(TEST_DB_PATH);
        Connection conn(db);
        conn.Query("CREATE TABLE t1 (x INTEGER)");
        conn.Query("INSERT INTO t1 VALUES (10), (20), (30)");
        conn.Query("CREATE TABLE t2 (name VARCHAR, active INTEGER)");
        conn.Query("INSERT INTO t2 VALUES ('foo', 1), ('bar', 0)");
    }

    {
        Database db(TEST_DB_PATH);
        Connection conn(db);

        auto r1 = conn.Query("SELECT COUNT(*) FROM t1");
        CHECK(r1.GetValue(0, 0).GetValue<int64_t>() == 3);

        auto r2 = conn.Query("SELECT COUNT(*) FROM t2");
        CHECK(r2.GetValue(0, 0).GetValue<int64_t>() == 2);

        auto r3 = conn.Query("SELECT name FROM t2 WHERE active = 1");
        CHECK(r3.RowCount() == 1);
        CHECK(r3.GetValue(0, 0).GetValue<std::string>() == "foo");
    }

    cleanup();
}

TEST_CASE("Persistence - null values") {
    cleanup();

    {
        Database db(TEST_DB_PATH);
        Connection conn(db);
        conn.Query("CREATE TABLE t (x INTEGER, y VARCHAR)");
        conn.Query("INSERT INTO t VALUES (1, 'hello')");
        conn.Query("INSERT INTO t VALUES (NULL, 'world')");
        conn.Query("INSERT INTO t VALUES (3, NULL)");
    }

    {
        Database db(TEST_DB_PATH);
        Connection conn(db);

        auto r = conn.Query("SELECT * FROM t");
        CHECK(r.RowCount() == 3);
        CHECK_FALSE(r.GetValue(0, 0).IsNull());
        CHECK(r.GetValue(1, 0).IsNull());
        CHECK_FALSE(r.GetValue(2, 0).IsNull());
        CHECK(r.GetValue(2, 1).IsNull());
    }

    cleanup();
}

TEST_CASE("Persistence - modify and re-save") {
    cleanup();

    {
        Database db(TEST_DB_PATH);
        Connection conn(db);
        conn.Query("CREATE TABLE t (x INTEGER)");
        conn.Query("INSERT INTO t VALUES (1), (2), (3)");
    }

    // Reopen, modify, close again.
    {
        Database db(TEST_DB_PATH);
        Connection conn(db);
        conn.Query("INSERT INTO t VALUES (4), (5)");
        conn.Query("DELETE FROM t WHERE x = 1");
    }

    // Verify final state.
    {
        Database db(TEST_DB_PATH);
        Connection conn(db);

        auto r = conn.Query("SELECT COUNT(*) FROM t");
        CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 4); // 2,3,4,5
    }

    cleanup();
}

TEST_CASE("Persistence - in-memory mode (no path)") {
    // Empty path = in-memory only, no persistence.
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (42)");

    auto r = conn.Query("SELECT x FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 42);
    // No file created, no crash on destruct.
}

TEST_CASE("Persistence - query after load") {
    cleanup();

    {
        Database db(TEST_DB_PATH);
        Connection conn(db);
        conn.Query("CREATE TABLE sales (product VARCHAR, amount INTEGER)");
        conn.Query("INSERT INTO sales VALUES ('A', 100), ('B', 200), ('A', 150)");
    }

    {
        Database db(TEST_DB_PATH);
        Connection conn(db);

        // Run aggregation on loaded data.
        auto r = conn.Query(
            "SELECT product, SUM(amount) FROM sales GROUP BY product");
        CHECK(r.RowCount() == 2);

        for (idx_t i = 0; i < r.RowCount(); i++) {
            auto product = r.GetValue(i, 0).GetValue<std::string>();
            auto total = r.GetValue(i, 1).GetValue<int64_t>();
            if (product == "A") CHECK(total == 250);
            if (product == "B") CHECK(total == 200);
        }
    }

    cleanup();
}

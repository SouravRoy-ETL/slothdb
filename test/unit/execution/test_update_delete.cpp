#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

TEST_CASE("UPDATE - basic") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR, score INTEGER)");
    conn.Query("INSERT INTO t VALUES (1, 'Alice', 80)");
    conn.Query("INSERT INTO t VALUES (2, 'Bob', 90)");
    conn.Query("INSERT INTO t VALUES (3, 'Charlie', 70)");

    conn.Query("UPDATE t SET score = 100 WHERE id = 2");

    auto r = conn.Query("SELECT score FROM t WHERE id = 2");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 100);

    // Other rows unchanged.
    r = conn.Query("SELECT score FROM t WHERE id = 1");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 80);
}

TEST_CASE("UPDATE - no WHERE (update all)") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    conn.Query("UPDATE t SET x = 0");

    auto r = conn.Query("SELECT x FROM t");
    CHECK(r.RowCount() == 3);
    for (idx_t i = 0; i < r.RowCount(); i++) {
        CHECK(r.GetValue(i, 0).GetValue<int32_t>() == 0);
    }
}

TEST_CASE("DELETE - with WHERE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3), (4), (5)");

    conn.Query("DELETE FROM t WHERE x > 3");

    auto r = conn.Query("SELECT x FROM t");
    CHECK(r.RowCount() == 3);
}

TEST_CASE("DELETE - all rows") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    conn.Query("DELETE FROM t");

    auto r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 0);
}

TEST_CASE("DELETE - then INSERT") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2)");
    conn.Query("DELETE FROM t WHERE x = 1");
    conn.Query("INSERT INTO t VALUES (10)");

    auto r = conn.Query("SELECT x FROM t");
    CHECK(r.RowCount() == 2);
}

TEST_CASE("UPDATE - multiple columns") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (a INTEGER, b VARCHAR)");
    conn.Query("INSERT INTO t VALUES (1, 'old')");

    conn.Query("UPDATE t SET a = 99, b = 'new' WHERE a = 1");

    auto r = conn.Query("SELECT a, b FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 99);
    CHECK(r.GetValue(0, 1).GetValue<std::string>() == "new");
}

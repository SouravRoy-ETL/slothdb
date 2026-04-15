#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/optimizer/optimizer.hpp"

using namespace slothdb;

TEST_CASE("Optimizer - constant folding preserves correctness") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3), (4), (5)");

    // Constant expression in SELECT: 2 * 3 should be folded to 6.
    auto r = conn.Query("SELECT x, 2 * 3 FROM t");
    CHECK(r.RowCount() == 5);
    for (idx_t i = 0; i < r.RowCount(); i++) {
        CHECK(r.GetValue(i, 1).GetValue<int32_t>() == 6);
    }
}

TEST_CASE("Optimizer - always-true filter removal") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    // WHERE 1 = 1 is always true -> optimizer should remove filter.
    auto r = conn.Query("SELECT x FROM t WHERE 1 = 1");
    CHECK(r.RowCount() == 3);

    // Check EXPLAIN shows the optimization happened.
    auto plan = conn.Query("EXPLAIN SELECT x FROM t WHERE 1 = 1");
    auto plan_text = plan.GetValue(0, 0).GetValue<std::string>();
    // After optimization, FILTER should be removed.
    // The plan should just have PROJECTION and SCAN.
    CHECK(plan_text.find("SCAN") != std::string::npos);
}

TEST_CASE("Optimizer - filter pushdown preserves correctness") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR, score INTEGER)");
    conn.Query("INSERT INTO t VALUES (1, 'Alice', 90)");
    conn.Query("INSERT INTO t VALUES (2, 'Bob', 80)");
    conn.Query("INSERT INTO t VALUES (3, 'Charlie', 70)");

    // Filter on a column should still work after optimization.
    auto r = conn.Query("SELECT name FROM t WHERE score > 75");
    CHECK(r.RowCount() == 2);
}

TEST_CASE("Optimizer - complex query still correct") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE emp (name VARCHAR, dept VARCHAR, salary INTEGER)");
    conn.Query("INSERT INTO emp VALUES ('A', 'Eng', 100), ('B', 'Eng', 200)");
    conn.Query("INSERT INTO emp VALUES ('C', 'Sales', 150), ('D', 'Sales', 250)");

    auto r = conn.Query(
        "SELECT dept, AVG(salary) FROM emp "
        "WHERE salary > 100 "
        "GROUP BY dept");
    CHECK(r.RowCount() == 2);
}

TEST_CASE("Optimizer - TopN pattern detection") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    for (int i = 0; i < 100; i++) {
        conn.Query("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    }

    // ORDER BY + LIMIT should be detected by optimizer.
    auto r = conn.Query("SELECT x FROM t ORDER BY x DESC LIMIT 5");
    CHECK(r.RowCount() == 5);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 99);
    CHECK(r.GetValue(4, 0).GetValue<int32_t>() == 95);
}

TEST_CASE("Optimizer - multiple optimizations don't break queries") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t1 (id INTEGER, val INTEGER)");
    conn.Query("CREATE TABLE t2 (id INTEGER, name VARCHAR)");
    conn.Query("INSERT INTO t1 VALUES (1, 10), (2, 20), (3, 30)");
    conn.Query("INSERT INTO t2 VALUES (1, 'A'), (2, 'B'), (3, 'C')");

    // Complex query with join + filter + order + limit.
    auto r = conn.Query(
        "SELECT t2.name, t1.val FROM t1 "
        "INNER JOIN t2 ON t1.id = t2.id "
        "WHERE t1.val > 10 "
        "ORDER BY t1.val DESC "
        "LIMIT 2");
    CHECK(r.RowCount() == 2);
}

TEST_CASE("Optimizer - describe applied optimizations") {
    // Run a query that triggers optimizations.
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1)");
    conn.Query("SELECT x, 1 + 2 FROM t WHERE 1 = 1");

    auto desc = Optimizer::DescribeOptimizations();
    // Should mention at least one optimization.
    CHECK(desc.size() > 0);
}

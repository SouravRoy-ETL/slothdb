#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

static void setup_employees(Connection &conn) {
    conn.Query("CREATE TABLE emp (name VARCHAR, dept VARCHAR, salary INTEGER)");
    conn.Query("INSERT INTO emp VALUES ('Alice', 'Engineering', 100)");
    conn.Query("INSERT INTO emp VALUES ('Bob', 'Engineering', 120)");
    conn.Query("INSERT INTO emp VALUES ('Charlie', 'Sales', 80)");
    conn.Query("INSERT INTO emp VALUES ('Diana', 'Sales', 90)");
    conn.Query("INSERT INTO emp VALUES ('Eve', 'Marketing', 95)");
}

TEST_CASE("Aggregation - COUNT(*) no group") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query("SELECT COUNT(*) FROM emp");
    CHECK(result.RowCount() == 1);
    CHECK(result.GetValue(0, 0).GetValue<int64_t>() == 5);
}

TEST_CASE("Aggregation - SUM no group") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query("SELECT SUM(salary) FROM emp");
    CHECK(result.RowCount() == 1);
    CHECK(result.GetValue(0, 0).GetValue<int64_t>() == 485);
}

TEST_CASE("Aggregation - AVG no group") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query("SELECT AVG(salary) FROM emp");
    CHECK(result.RowCount() == 1);
    CHECK(result.GetValue(0, 0).GetValue<double>() == doctest::Approx(97.0));
}

TEST_CASE("Aggregation - MIN and MAX no group") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query("SELECT MIN(salary), MAX(salary) FROM emp");
    CHECK(result.RowCount() == 1);
    CHECK(result.GetValue(0, 0).GetValue<int32_t>() == 80);
    CHECK(result.GetValue(0, 1).GetValue<int32_t>() == 120);
}

TEST_CASE("Aggregation - GROUP BY with COUNT") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query("SELECT dept, COUNT(*) FROM emp GROUP BY dept");
    CHECK(result.RowCount() == 3); // Engineering, Sales, Marketing

    // Verify each group has expected count.
    bool found_eng = false, found_sales = false, found_mkt = false;
    for (idx_t i = 0; i < result.RowCount(); i++) {
        auto dept = result.GetValue(i, 0).GetValue<std::string>();
        auto cnt = result.GetValue(i, 1).GetValue<int64_t>();
        if (dept == "Engineering") { CHECK(cnt == 2); found_eng = true; }
        if (dept == "Sales") { CHECK(cnt == 2); found_sales = true; }
        if (dept == "Marketing") { CHECK(cnt == 1); found_mkt = true; }
    }
    CHECK(found_eng);
    CHECK(found_sales);
    CHECK(found_mkt);
}

TEST_CASE("Aggregation - GROUP BY with SUM") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query("SELECT dept, SUM(salary) FROM emp GROUP BY dept");
    CHECK(result.RowCount() == 3);

    for (idx_t i = 0; i < result.RowCount(); i++) {
        auto dept = result.GetValue(i, 0).GetValue<std::string>();
        auto total = result.GetValue(i, 1).GetValue<int64_t>();
        if (dept == "Engineering") CHECK(total == 220);
        if (dept == "Sales") CHECK(total == 170);
        if (dept == "Marketing") CHECK(total == 95);
    }
}

TEST_CASE("Aggregation - GROUP BY with AVG") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query("SELECT dept, AVG(salary) FROM emp GROUP BY dept");
    for (idx_t i = 0; i < result.RowCount(); i++) {
        auto dept = result.GetValue(i, 0).GetValue<std::string>();
        auto avg = result.GetValue(i, 1).GetValue<double>();
        if (dept == "Engineering") CHECK(avg == doctest::Approx(110.0));
        if (dept == "Sales") CHECK(avg == doctest::Approx(85.0));
    }
}

TEST_CASE("Aggregation - multiple aggregates") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query(
        "SELECT dept, COUNT(*), SUM(salary), MIN(salary), MAX(salary) "
        "FROM emp GROUP BY dept");
    CHECK(result.RowCount() == 3);
    CHECK(result.ColumnCount() == 5);
}

TEST_CASE("Aggregation - empty table") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE empty_t (x INTEGER)");

    auto result = conn.Query("SELECT COUNT(*) FROM empty_t");
    CHECK(result.RowCount() == 1);
    CHECK(result.GetValue(0, 0).GetValue<int64_t>() == 0);
}

// Regression: ROUND(AVG(x)) used to throw "Function execution for: AVG"
// because the planner only hoisted top-level aggregate calls. Anything
// nested inside a scalar wrapper (ROUND, CAST, arithmetic) fell through
// and AVG hit the scalar dispatcher.
TEST_CASE("Aggregation - nested aggregate inside scalar function") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query(
        "SELECT dept, ROUND(AVG(salary)) AS avg_s "
        "FROM emp GROUP BY dept ORDER BY avg_s DESC");
    CHECK(result.RowCount() == 3);
    // Engineering: round((100+120)/2)=110; Eve(Marketing)=95; Sales=85.
    CHECK(result.GetValue(0, 0).GetValue<std::string>() == "Engineering");
    CHECK(result.GetValue(0, 1).GetValue<double>() == doctest::Approx(110.0));
    CHECK(result.GetValue(1, 0).GetValue<std::string>() == "Marketing");
    CHECK(result.GetValue(1, 1).GetValue<double>() == doctest::Approx(95.0));
    CHECK(result.GetValue(2, 0).GetValue<std::string>() == "Sales");
    CHECK(result.GetValue(2, 1).GetValue<double>() == doctest::Approx(85.0));
}

TEST_CASE("Aggregation - aggregate with arithmetic literal") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query(
        "SELECT dept, AVG(salary) + 1 AS bumped "
        "FROM emp GROUP BY dept ORDER BY bumped DESC");
    CHECK(result.RowCount() == 3);
    CHECK(result.GetValue(0, 1).GetValue<double>() == doctest::Approx(111.0));
    CHECK(result.GetValue(1, 1).GetValue<double>() == doctest::Approx(96.0));
    CHECK(result.GetValue(2, 1).GetValue<double>() == doctest::Approx(86.0));
}

TEST_CASE("Aggregation - two aggregates dividing each other") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query(
        "SELECT dept, SUM(salary) / COUNT(*) AS per_head "
        "FROM emp GROUP BY dept ORDER BY per_head DESC");
    CHECK(result.RowCount() == 3);
    // Engineering 220/2 = 110, Marketing 95/1 = 95, Sales 170/2 = 85.
    CHECK(result.GetValue(0, 0).GetValue<std::string>() == "Engineering");
    CHECK(result.GetValue(1, 0).GetValue<std::string>() == "Marketing");
    CHECK(result.GetValue(2, 0).GetValue<std::string>() == "Sales");
}

TEST_CASE("Aggregation - cast of aggregate") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query(
        "SELECT dept, CAST(SUM(salary) AS DOUBLE) AS s_sum "
        "FROM emp GROUP BY dept ORDER BY s_sum DESC");
    CHECK(result.RowCount() == 3);
    CHECK(result.GetValue(0, 1).GetValue<double>() == doctest::Approx(220.0));
}

// Regression: ORDER BY by aggregate alias used to silently sort by col 0.
TEST_CASE("Aggregation - ORDER BY aggregate alias") {
    Database db;
    Connection conn(db);
    setup_employees(conn);

    auto result = conn.Query(
        "SELECT dept, COUNT(*) AS cnt FROM emp GROUP BY dept ORDER BY cnt DESC, dept");
    CHECK(result.RowCount() == 3);
    // Engineering=2 and Sales=2 tie on cnt; tie-break dept ASC -> Engineering, Sales.
    // Marketing=1 last.
    CHECK(result.GetValue(0, 0).GetValue<std::string>() == "Engineering");
    CHECK(result.GetValue(0, 1).GetValue<int64_t>() == 2);
    CHECK(result.GetValue(1, 0).GetValue<std::string>() == "Sales");
    CHECK(result.GetValue(1, 1).GetValue<int64_t>() == 2);
    CHECK(result.GetValue(2, 0).GetValue<std::string>() == "Marketing");
    CHECK(result.GetValue(2, 1).GetValue<int64_t>() == 1);
}

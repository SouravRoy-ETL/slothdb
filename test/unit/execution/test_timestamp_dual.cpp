#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

// ============================================================================
// DUAL / SELECT without FROM
// ============================================================================

TEST_CASE("DUAL - SELECT constant without FROM") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT 42");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 42);
}

TEST_CASE("DUAL - SELECT string constant") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT 'hello world'");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "hello world");
}

TEST_CASE("DUAL - SELECT expression") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT 2 + 3");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 5);
}

// ============================================================================
// Timestamp functions
// ============================================================================

TEST_CASE("Timestamp - NOW() returns value") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT NOW()");
    CHECK(r.RowCount() == 1);
    // NOW() returns microseconds since epoch. Should be > 2020-01-01.
    auto val = r.GetValue(0, 0).GetValue<int64_t>();
    CHECK(val > 1577836800000000LL); // 2020-01-01 in micros
}

TEST_CASE("Timestamp - CURRENT_TIMESTAMP") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT CURRENT_TIMESTAMP");
    CHECK(r.RowCount() == 1);
    auto val = r.GetValue(0, 0).GetValue<int64_t>();
    CHECK(val > 1577836800000000LL);
}

TEST_CASE("Timestamp - CURRENT_DATE") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT CURRENT_DATE");
    CHECK(r.RowCount() == 1);
    auto val = r.GetValue(0, 0).GetValue<int32_t>();
    CHECK(val >= 20200101); // YYYYMMDD format
    CHECK(val <= 20991231);
}

TEST_CASE("Timestamp - EXTRACT YEAR/MONTH/DAY") {
    Database db;
    Connection conn(db);

    // 2024-06-15 12:30:00 UTC = 1718451000 epoch seconds
    // In microseconds: 1718451000000000
    conn.Query("CREATE TABLE ts (t BIGINT)");
    conn.Query("INSERT INTO ts VALUES (1718451000000000)");

    auto r = conn.Query("SELECT EXTRACT(YEAR FROM t) FROM ts");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 2024);

    r = conn.Query("SELECT EXTRACT(MONTH FROM t) FROM ts");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 6);

    r = conn.Query("SELECT EXTRACT(DAY FROM t) FROM ts");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 15);

    r = conn.Query("SELECT EXTRACT(HOUR FROM t) FROM ts");
    auto hour = r.GetValue(0, 0).GetValue<int64_t>();
    CHECK(hour >= 0);
    CHECK(hour <= 23);
}

TEST_CASE("Timestamp - TO_TIMESTAMP") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("SELECT TO_TIMESTAMP(1718451000)");
    CHECK(r.RowCount() == 1);
    auto micros = r.GetValue(0, 0).GetValue<int64_t>();
    CHECK(micros == 1718451000000000LL);
}

TEST_CASE("Timestamp - NOW() with table") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1)");

    auto r = conn.Query("SELECT x, NOW() FROM t");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 1);
    CHECK(r.GetValue(0, 1).GetValue<int64_t>() > 0);
}

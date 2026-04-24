#ifdef _MSC_VER
#  pragma warning(disable: 4996)  // std::getenv on MSVC
#endif

#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

#include <cstdlib>

using namespace slothdb;

// These tests hit the real network. They are gated on the environment variable
// SLOTHDB_HTTPFS_ONLINE so offline CI skips them cleanly. To run locally:
//   set SLOTHDB_HTTPFS_ONLINE=1  (Windows)
//   export SLOTHDB_HTTPFS_ONLINE=1  (POSIX)

static bool online_tests_enabled() {
    const char *v = std::getenv("SLOTHDB_HTTPFS_ONLINE");
    return v && std::string(v) == "1";
}

// Stable test fixture: employees.csv lives in the examples/ directory on main.
// 12 rows, columns: name, department, salary, hire_year.
static constexpr const char *EMPLOYEES_URL =
    "https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/examples/employees.csv";

TEST_CASE("HTTPFS - read_csv over https returns correct row count") {
    if (!online_tests_enabled()) return;

    Database db; Connection conn(db);
    std::string sql = "SELECT COUNT(*) FROM '";
    sql += EMPLOYEES_URL;
    sql += "'";
    auto r = conn.Query(sql);
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 12);
}

TEST_CASE("HTTPFS - read_csv projection over https") {
    if (!online_tests_enabled()) return;

    Database db; Connection conn(db);
    std::string sql = "SELECT department, COUNT(*) FROM '";
    sql += EMPLOYEES_URL;
    sql += "' GROUP BY department ORDER BY department";
    auto r = conn.Query(sql);
    CHECK(r.RowCount() >= 3);  // employees.csv has Engineering, Sales, Marketing
    CHECK(r.ColumnCount() == 2);
}

TEST_CASE("HTTPFS - explicit read_csv() call accepts an https URL") {
    if (!online_tests_enabled()) return;

    Database db; Connection conn(db);
    std::string sql = "SELECT name FROM read_csv('";
    sql += EMPLOYEES_URL;
    sql += "') WHERE salary > 100000";
    auto r = conn.Query(sql);
    // Employees with salary > 100000 in the fixture: Alice (120k), Bob (110k),
    // Diana (105k), Frank (130k), Grace (115k), Ivy (105k), Karen (110k),
    // Leo (125k) -> 8 rows.
    CHECK(r.RowCount() == 8);
}

TEST_CASE("HTTPFS - non-URL paths still work unchanged (regression)") {
    // Explicit local-path regression: the helper must not touch non-URL inputs.
    // This test runs unconditionally (no network).
    Database db; Connection conn(db);
    // Use a path that does NOT exist; we just want to verify the URL check
    // doesn't accidentally treat it as remote. Should raise IOException for
    // file-not-found, not for download-failed.
    try {
        conn.Query("SELECT * FROM '/nonexistent/file.csv'");
    } catch (const std::exception &e) {
        std::string msg = e.what();
        // The message should NOT mention "Failed to download" (that would
        // indicate the URL branch was taken for a local path).
        CHECK(msg.find("Failed to download") == std::string::npos);
    }
}

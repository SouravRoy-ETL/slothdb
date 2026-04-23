#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace slothdb;

namespace {
// Write a full file replacing its previous contents. Used by LIVE VIEW
// tests — the mtime change is what the cache check observes.
void WriteFile(const std::string &path, const std::string &content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}
} // namespace

// ============================================================================
// BETWEEN
// ============================================================================

TEST_CASE("BETWEEN - basic") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (3), (5), (7), (9)");

    auto r = conn.Query("SELECT x FROM t WHERE x BETWEEN 3 AND 7");
    CHECK(r.RowCount() == 3); // 3, 5, 7
}

TEST_CASE("BETWEEN - strings") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (name VARCHAR)");
    conn.Query("INSERT INTO t VALUES ('Alice'), ('Bob'), ('Charlie'), ('Diana')");

    auto r = conn.Query("SELECT name FROM t WHERE name BETWEEN 'B' AND 'D'");
    CHECK(r.RowCount() == 2); // Bob, Charlie
}

// ============================================================================
// EXPLAIN
// ============================================================================

TEST_CASE("EXPLAIN - shows plan") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");

    auto r = conn.Query("EXPLAIN SELECT x FROM t WHERE x > 5");
    CHECK(r.RowCount() == 1);
    CHECK(r.column_names[0] == "plan");
    auto plan = r.GetValue(0, 0).GetValue<std::string>();
    CHECK(plan.find("FILTER") != std::string::npos);
    CHECK(plan.find("SCAN") != std::string::npos);
    CHECK(plan.find("PROJECTION") != std::string::npos);
}

TEST_CASE("EXPLAIN - aggregate plan") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER, y VARCHAR)");

    auto r = conn.Query("EXPLAIN SELECT y, COUNT(*) FROM t GROUP BY y");
    auto plan = r.GetValue(0, 0).GetValue<std::string>();
    CHECK(plan.find("AGGREGATE") != std::string::npos);
}

// ============================================================================
// DESCRIBE
// ============================================================================

TEST_CASE("DESCRIBE - table name") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR, price DOUBLE)");

    auto r = conn.Query("DESCRIBE t");
    CHECK(r.RowCount() == 3);
    CHECK(r.column_names.size() == 6);
    CHECK(r.column_names[0] == "column_name");
    CHECK(r.column_names[1] == "column_type");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "id");
    CHECK(r.GetValue(0, 1).GetValue<std::string>() == "INTEGER");
    CHECK(r.GetValue(1, 0).GetValue<std::string>() == "name");
    CHECK(r.GetValue(1, 1).GetValue<std::string>() == "VARCHAR");
    CHECK(r.GetValue(2, 0).GetValue<std::string>() == "price");
    CHECK(r.GetValue(2, 1).GetValue<std::string>() == "DOUBLE");
}

TEST_CASE("DESCRIBE - SELECT subquery") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER, y VARCHAR)");

    auto r = conn.Query("DESCRIBE SELECT x, UPPER(y) AS upper_y FROM t");
    CHECK(r.RowCount() == 2);
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "x");
    CHECK(r.GetValue(0, 1).GetValue<std::string>() == "INTEGER");
    CHECK(r.GetValue(1, 0).GetValue<std::string>() == "upper_y");
    CHECK(r.GetValue(1, 1).GetValue<std::string>() == "VARCHAR");
}

TEST_CASE("DESCRIBE - aggregate output") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER, y VARCHAR)");

    auto r = conn.Query("DESCRIBE SELECT y, COUNT(*) AS cnt FROM t GROUP BY y");
    CHECK(r.RowCount() == 2);
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "y");
    CHECK(r.GetValue(1, 0).GetValue<std::string>() == "cnt");
    CHECK(r.GetValue(1, 1).GetValue<std::string>() == "BIGINT");
}

// ============================================================================
// PRAGMA
// ============================================================================

TEST_CASE("PRAGMA table_info - returns columns") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR)");

    auto r = conn.Query("PRAGMA table_info('t')");
    CHECK(r.RowCount() == 2);
    CHECK(r.column_names.size() == 6);
    CHECK(r.column_names[0] == "cid");
    CHECK(r.column_names[1] == "name");
    CHECK(r.column_names[2] == "type");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 0);
    CHECK(r.GetValue(0, 1).GetValue<std::string>() == "id");
    CHECK(r.GetValue(0, 2).GetValue<std::string>() == "INTEGER");
    CHECK(r.GetValue(1, 0).GetValue<int32_t>() == 1);
    CHECK(r.GetValue(1, 1).GetValue<std::string>() == "name");
    CHECK(r.GetValue(1, 2).GetValue<std::string>() == "VARCHAR");
}

TEST_CASE("PRAGMA database_list - returns memory") {
    Database db;
    Connection conn(db);

    auto r = conn.Query("PRAGMA database_list");
    CHECK(r.RowCount() == 1);
    CHECK(r.column_names[1] == "name");
    CHECK(r.GetValue(0, 1).GetValue<std::string>() == "memory");
}

TEST_CASE("PRAGMA table_info - missing table errors") {
    Database db;
    Connection conn(db);
    CHECK_THROWS(conn.Query("PRAGMA table_info('does_not_exist')"));
}

// ============================================================================
// VARCHAR(n) length enforcement
// ============================================================================

TEST_CASE("VARCHAR(n) - accepts strings within limit") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (code VARCHAR(3))");
    conn.Query("INSERT INTO t VALUES ('a'), ('ab'), ('abc')");
    auto r = conn.Query("SELECT * FROM t");
    CHECK(r.RowCount() == 3);
}

TEST_CASE("VARCHAR(n) - rejects over-long strings") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (code VARCHAR(3))");
    CHECK_THROWS(conn.Query("INSERT INTO t VALUES ('too_long')"));
}

TEST_CASE("VARCHAR(n) - preserved through DESCRIBE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (code VARCHAR(5))");
    auto r = conn.Query("DESCRIBE t");
    CHECK(r.GetValue(0, 1).GetValue<std::string>() == "VARCHAR(5)");
}

TEST_CASE("VARCHAR without length stays unbounded") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (v VARCHAR)");
    // A long string is accepted because no length was declared.
    conn.Query("INSERT INTO t VALUES ('"
               "abcdefghijklmnopqrstuvwxyz0123456789')");
    auto r = conn.Query("SELECT * FROM t");
    CHECK(r.RowCount() == 1);
}

// ============================================================================
// CREATE LIVE VIEW
// ============================================================================

TEST_CASE("LIVE VIEW - refreshes when file mtime changes") {
    auto dir = std::filesystem::temp_directory_path() / "slothdb_live_view";
    std::filesystem::create_directories(dir);
    auto path = dir / "data.csv";
    WriteFile(path.string(), "k,label\n1,a\n2,b\n");

    Database db;
    Connection conn(db);

    auto sql = "CREATE LIVE VIEW app AS SELECT * FROM '" + path.string() + "'";
    conn.Query(sql);

    auto before = conn.Query("SELECT COUNT(*) FROM app");
    CHECK(before.GetValue(0, 0).GetValue<int64_t>() == 2);

    // Sleep enough for a detectable mtime change on all filesystems.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    WriteFile(path.string(), "k,label\n1,a\n2,b\n3,c\n4,d\n");

    auto after = conn.Query("SELECT COUNT(*) FROM app");
    CHECK(after.GetValue(0, 0).GetValue<int64_t>() == 4);

    std::filesystem::remove(path);
}

TEST_CASE("LIVE VIEW - cache hit when file unchanged") {
    auto dir = std::filesystem::temp_directory_path() / "slothdb_live_view_cache";
    std::filesystem::create_directories(dir);
    auto path = dir / "data.csv";
    WriteFile(path.string(), "k\n1\n2\n3\n");

    Database db;
    Connection conn(db);

    conn.Query("CREATE LIVE VIEW v AS SELECT * FROM '" + path.string() + "'");

    // Two back-to-back SELECTs with no file change should see identical
    // row counts — the second one reads from the cache.
    auto r1 = conn.Query("SELECT COUNT(*) FROM v");
    auto r2 = conn.Query("SELECT COUNT(*) FROM v");
    CHECK(r1.GetValue(0, 0).GetValue<int64_t>() == 3);
    CHECK(r2.GetValue(0, 0).GetValue<int64_t>() == 3);

    std::filesystem::remove(path);
}

TEST_CASE("LIVE VIEW - rejects missing file source") {
    Database db;
    Connection conn(db);
    // No FROM clause with a file literal — should error.
    CHECK_THROWS(conn.Query(
        "CREATE LIVE VIEW bad AS SELECT 1 AS x"));
}

TEST_CASE("Non-live VIEW still re-executes unconditionally") {
    // Regression: the live-view path must not accidentally gate non-live
    // views on mtime.
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2)");
    conn.Query("CREATE VIEW v AS SELECT * FROM t");
    auto r1 = conn.Query("SELECT COUNT(*) FROM v");
    CHECK(r1.GetValue(0, 0).GetValue<int64_t>() == 2);
    conn.Query("INSERT INTO t VALUES (3)");
    auto r2 = conn.Query("SELECT COUNT(*) FROM v");
    CHECK(r2.GetValue(0, 0).GetValue<int64_t>() == 3);
}

TEST_CASE("LIVE VIEW v2 - incremental append on tail-appended CSV") {
    auto dir = std::filesystem::temp_directory_path() / "slothdb_live_view_incr";
    std::filesystem::create_directories(dir);
    auto path = dir / "log.csv";
    // Header plus 3 rows.
    WriteFile(path.string(), "ts,level\n1,info\n2,warn\n3,error\n");

    Database db;
    Connection conn(db);
    conn.Query("CREATE LIVE VIEW app AS SELECT * FROM '" + path.string() + "'");
    CHECK(conn.Query("SELECT COUNT(*) FROM app").GetValue(0, 0).GetValue<int64_t>() == 3);

    // Append (not overwrite) 2 new rows. The append path in expand_view
    // should pick up only the tail, leaving the cached 3 untouched.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    {
        std::ofstream f(path.string(), std::ios::binary | std::ios::app);
        f << "4,info\n5,error\n";
    }
    CHECK(conn.Query("SELECT COUNT(*) FROM app").GetValue(0, 0).GetValue<int64_t>() == 5);

    // Values preserved across the append — both old and new rows present.
    auto r = conn.Query("SELECT ts FROM app ORDER BY ts");
    CHECK(r.RowCount() == 5);
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1);
    CHECK(r.GetValue(4, 0).GetValue<int64_t>() == 5);

    std::filesystem::remove(path);
}

TEST_CASE("LIVE VIEW v2 - rewrite-in-place triggers full rescan") {
    // If the first 64 bytes change, it's not an append — the file was
    // rewritten. Fall back to full rescan even if the new size >= old.
    auto dir = std::filesystem::temp_directory_path() / "slothdb_live_view_rewrite";
    std::filesystem::create_directories(dir);
    auto path = dir / "data.csv";
    WriteFile(path.string(), "k\n1\n2\n3\n");

    Database db;
    Connection conn(db);
    conn.Query("CREATE LIVE VIEW v AS SELECT * FROM '" + path.string() + "'");
    CHECK(conn.Query("SELECT COUNT(*) FROM v").GetValue(0, 0).GetValue<int64_t>() == 3);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    // Different header — entirely rewritten file with same column count.
    WriteFile(path.string(), "kk\n10\n20\n30\n40\n50\n");

    // Result should be exactly the 5 new rows — no accidental append of
    // old rows on top.
    auto r = conn.Query("SELECT COUNT(*) FROM v");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 5);

    std::filesystem::remove(path);
}

TEST_CASE("LIVE VIEW v2 - truncated file triggers full rescan") {
    auto dir = std::filesystem::temp_directory_path() / "slothdb_live_view_trunc";
    std::filesystem::create_directories(dir);
    auto path = dir / "data.csv";
    WriteFile(path.string(), "k\n1\n2\n3\n4\n5\n");

    Database db;
    Connection conn(db);
    conn.Query("CREATE LIVE VIEW v AS SELECT * FROM '" + path.string() + "'");
    CHECK(conn.Query("SELECT COUNT(*) FROM v").GetValue(0, 0).GetValue<int64_t>() == 5);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    WriteFile(path.string(), "k\n1\n2\n"); // truncated
    CHECK(conn.Query("SELECT COUNT(*) FROM v").GetValue(0, 0).GetValue<int64_t>() == 2);

    std::filesystem::remove(path);
}

TEST_CASE("LIVE VIEW v2 - view with WHERE falls back to full rescan") {
    // WHERE makes the view non-incremental — the cache is filtered rows
    // and appending parse-new-bytes would skip the filter. Confirm the
    // whole pipeline still produces correct output (via the full-rescan
    // branch) when the file grows.
    auto dir = std::filesystem::temp_directory_path() / "slothdb_live_view_where";
    std::filesystem::create_directories(dir);
    auto path = dir / "data.csv";
    WriteFile(path.string(), "k,lvl\n1,info\n2,error\n3,info\n");

    Database db;
    Connection conn(db);
    conn.Query("CREATE LIVE VIEW errors AS SELECT * FROM '"
               + path.string() + "' WHERE lvl = 'error'");
    CHECK(conn.Query("SELECT COUNT(*) FROM errors").GetValue(0, 0).GetValue<int64_t>() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    {
        std::ofstream f(path.string(), std::ios::binary | std::ios::app);
        f << "4,error\n5,info\n6,error\n";
    }
    // 3 errors total after append; WHERE is applied via full rescan.
    CHECK(conn.Query("SELECT COUNT(*) FROM errors").GetValue(0, 0).GetValue<int64_t>() == 3);

    std::filesystem::remove(path);
}

// ============================================================================
// CREATE VIEW
// ============================================================================

TEST_CASE("CREATE VIEW - basic") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE employees (name VARCHAR, dept VARCHAR, salary INTEGER)");
    conn.Query("INSERT INTO employees VALUES ('Alice', 'Eng', 100), ('Bob', 'Sales', 90)");

    conn.Query("CREATE VIEW eng_view AS SELECT name, salary FROM employees WHERE dept = 'Eng'");

    auto r = conn.Query("SELECT * FROM eng_view");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "Alice");
}

TEST_CASE("CREATE VIEW - virtual re-execution") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE data (id INTEGER, val VARCHAR)");
    conn.Query("INSERT INTO data VALUES (1, 'a'), (2, 'b')");

    // Create a view over the table.
    conn.Query("CREATE VIEW data_view AS SELECT * FROM data");

    auto r = conn.Query("SELECT COUNT(*) FROM data_view");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 2);

    // Insert more data into the underlying table.
    conn.Query("INSERT INTO data VALUES (3, 'c'), (4, 'd')");

    // The view should reflect the updated data.
    r = conn.Query("SELECT COUNT(*) FROM data_view");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 4);

    // Delete from underlying table.
    conn.Query("DELETE FROM data WHERE id > 2");

    r = conn.Query("SELECT COUNT(*) FROM data_view");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 2);
}

TEST_CASE("CREATE OR REPLACE VIEW") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    conn.Query("CREATE VIEW v AS SELECT x FROM t WHERE x > 1");
    auto r = conn.Query("SELECT * FROM v");
    CHECK(r.RowCount() == 2);

    conn.Query("CREATE OR REPLACE VIEW v AS SELECT x FROM t WHERE x > 2");
    r = conn.Query("SELECT * FROM v");
    CHECK(r.RowCount() == 1);
}

// ============================================================================
// TRUNCATE
// ============================================================================

TEST_CASE("TRUNCATE TABLE") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1), (2), (3)");

    auto r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 3);

    conn.Query("TRUNCATE TABLE t");

    r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 0);

    // Table still exists, can insert again.
    conn.Query("INSERT INTO t VALUES (10)");
    r = conn.Query("SELECT x FROM t");
    CHECK(r.RowCount() == 1);
}

TEST_CASE("TRUNCATE without TABLE keyword") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1)");

    conn.Query("TRUNCATE t");
    auto r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 0);
}

// ============================================================================
// Combined queries
// ============================================================================

TEST_CASE("Complex - CTE + window + qualify") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE sales (product VARCHAR, region VARCHAR, amount INTEGER)");
    conn.Query("INSERT INTO sales VALUES ('A', 'East', 100)");
    conn.Query("INSERT INTO sales VALUES ('B', 'East', 200)");
    conn.Query("INSERT INTO sales VALUES ('A', 'West', 150)");
    conn.Query("INSERT INTO sales VALUES ('B', 'West', 50)");

    auto r = conn.Query(
        "WITH ranked AS ("
        "  SELECT product, region, amount, "
        "    ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount DESC) "
        "  FROM sales"
        ") "
        "SELECT * FROM ranked");
    CHECK(r.RowCount() == 4);
}

TEST_CASE("Complex - multiple aggregates + BETWEEN") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE scores (name VARCHAR, score INTEGER)");
    conn.Query("INSERT INTO scores VALUES ('A', 85), ('B', 92), ('C', 78), ('D', 95), ('E', 88)");

    auto r = conn.Query(
        "SELECT COUNT(*), AVG(score), MIN(score), MAX(score) "
        "FROM scores WHERE score BETWEEN 80 AND 95");
    CHECK(r.RowCount() == 1);
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 4); // 85, 92, 88, 95
}

#include "doctest.h"
#include "slothdb/common/thread_pool.hpp"
#include "slothdb/execution/parallel_executor.hpp"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include <atomic>

using namespace slothdb;

// ============================================================================
// Thread Pool
// ============================================================================

TEST_CASE("ThreadPool - basic task execution") {
    ThreadPool pool(4);
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 100; i++) {
        futures.push_back(pool.Submit([&counter]() {
            counter.fetch_add(1);
        }));
    }

    for (auto &f : futures) f.get();
    CHECK(counter.load() == 100);
}

TEST_CASE("ThreadPool - returns results") {
    ThreadPool pool(2);

    auto f1 = pool.Submit([]() { return 42; });
    auto f2 = pool.Submit([]() { return 100; });

    CHECK(f1.get() == 42);
    CHECK(f2.get() == 100);
}

TEST_CASE("ThreadPool - hardware concurrency detection") {
    ThreadPool pool;
    CHECK(pool.NumThreads() > 0);
}

// ============================================================================
// Parallel Scan
// ============================================================================

TEST_CASE("ParallelScan - large table correctness") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE big (id INTEGER, val DOUBLE)");

    // Insert 200K+ rows to get multiple row groups.
    idx_t target = 200000;
    idx_t inserted = 0;
    while (inserted < target) {
        DataChunk chunk;
        chunk.Initialize({LogicalType::INTEGER(), LogicalType::DOUBLE()});
        idx_t batch = std::min(VECTOR_SIZE, target - inserted);
        for (idx_t i = 0; i < batch; i++) {
            chunk.SetValue(0, i, Value::INTEGER(static_cast<int32_t>(inserted + i)));
            chunk.SetValue(1, i, Value::DOUBLE(static_cast<double>(inserted + i) * 1.5));
        }
        conn.Append("big", chunk);
        inserted += batch;
    }

    // Verify row count.
    auto r = conn.Query("SELECT COUNT(*) FROM big");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == static_cast<int64_t>(target));

    // Parallel scan should produce same count.
    auto *entry = db.GetCatalog().GetTable("big");
    auto &storage = entry->GetStorage();

    auto result_types = storage.GetTypes();
    auto rows = ParallelExecutor::ParallelScanFilter(
        storage, nullptr, nullptr, result_types);
    CHECK(rows.size() == target);
}

TEST_CASE("ParallelScan - with filter") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE nums (x INTEGER)");

    idx_t target = 150000;
    idx_t inserted = 0;
    while (inserted < target) {
        DataChunk chunk;
        chunk.Initialize({LogicalType::INTEGER()});
        idx_t batch = std::min(VECTOR_SIZE, target - inserted);
        for (idx_t i = 0; i < batch; i++) {
            chunk.SetValue(0, i, Value::INTEGER(static_cast<int32_t>(inserted + i)));
        }
        conn.Append("nums", chunk);
        inserted += batch;
    }

    // Count rows where x >= 100000.
    auto r = conn.Query("SELECT COUNT(*) FROM nums WHERE x >= 100000");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 50000);
}

TEST_CASE("ParallelScan - aggregation over large table") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE vals (x INTEGER)");

    // Insert 1..10000.
    for (int batch = 0; batch < 10; batch++) {
        DataChunk chunk;
        chunk.Initialize({LogicalType::INTEGER()});
        for (idx_t i = 0; i < 1000; i++) {
            chunk.SetValue(0, i, Value::INTEGER(static_cast<int32_t>(batch * 1000 + i + 1)));
        }
        conn.Append("vals", chunk);
    }

    auto r = conn.Query("SELECT SUM(x) FROM vals");
    // SUM(1..10000) = 10000 * 10001 / 2 = 50005000
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 50005000);
}

// ============================================================================
// Parallel correctness: multi-threaded produces same results as single-threaded
// ============================================================================

TEST_CASE("Parallel - SQL query on large table") {
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE employees (id INTEGER, dept VARCHAR, salary INTEGER)");

    // Insert 50K employees across 5 departments.
    for (int i = 0; i < 50000; i++) {
        std::string dept;
        switch (i % 5) {
        case 0: dept = "Eng"; break;
        case 1: dept = "Sales"; break;
        case 2: dept = "Marketing"; break;
        case 3: dept = "HR"; break;
        case 4: dept = "Finance"; break;
        }
        DataChunk chunk;
        chunk.Initialize({LogicalType::INTEGER(), LogicalType::VARCHAR(), LogicalType::INTEGER()});
        chunk.SetValue(0, 0, Value::INTEGER(i));
        chunk.SetValue(1, 0, Value::VARCHAR(dept));
        chunk.SetValue(2, 0, Value::INTEGER(50000 + (i % 100) * 1000));
        conn.Append("employees", chunk);
    }

    // GROUP BY should produce exactly 5 departments.
    auto r = conn.Query("SELECT dept, COUNT(*) FROM employees GROUP BY dept");
    CHECK(r.RowCount() == 5);

    // Each department should have 10000 employees.
    for (idx_t i = 0; i < r.RowCount(); i++) {
        CHECK(r.GetValue(i, 1).GetValue<int64_t>() == 10000);
    }
}

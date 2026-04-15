#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/common/exception.hpp"

using namespace slothdb;

TEST_CASE("DataTable - create, insert, and scan") {
    Database db;
    Connection conn(db);

    conn.CreateTable("t1", {
        {"id", LogicalType::INTEGER()},
        {"value", LogicalType::DOUBLE()},
    });

    // Insert 100 rows.
    DataChunk chunk;
    chunk.Initialize({LogicalType::INTEGER(), LogicalType::DOUBLE()});
    for (idx_t i = 0; i < 100; i++) {
        chunk.SetValue(0, i, Value::INTEGER(static_cast<int32_t>(i)));
        chunk.SetValue(1, i, Value::DOUBLE(static_cast<double>(i) * 2.5));
    }
    conn.Append("t1", chunk);

    // Scan and verify.
    idx_t total_rows = 0;
    conn.Scan("t1", [&](DataChunk &result) {
        for (idx_t i = 0; i < result.size(); i++) {
            auto id = result.GetValue(0, i).GetValue<int32_t>();
            auto val = result.GetValue(1, i).GetValue<double>();
            CHECK(id == static_cast<int32_t>(total_rows + i));
            CHECK(val == doctest::Approx(static_cast<double>(total_rows + i) * 2.5));
        }
        total_rows += result.size();
    });
    CHECK(total_rows == 100);
}

TEST_CASE("DataTable - VARCHAR columns") {
    Database db;
    Connection conn(db);

    conn.CreateTable("names", {
        {"name", LogicalType::VARCHAR()},
    });

    DataChunk chunk;
    chunk.Initialize({LogicalType::VARCHAR()});
    chunk.SetValue(0, 0, Value::VARCHAR("Alice"));
    chunk.SetValue(0, 1, Value::VARCHAR("Bob"));
    chunk.SetValue(0, 2, Value::VARCHAR("Charlie has a very long name that exceeds inline storage"));
    conn.Append("names", chunk);

    idx_t row = 0;
    conn.Scan("names", [&](DataChunk &result) {
        CHECK(result.GetValue(0, 0).GetValue<std::string>() == "Alice");
        CHECK(result.GetValue(0, 1).GetValue<std::string>() == "Bob");
        CHECK(result.GetValue(0, 2).GetValue<std::string>() ==
              "Charlie has a very long name that exceeds inline storage");
        row += result.size();
    });
    CHECK(row == 3);
}

TEST_CASE("DataTable - null values") {
    Database db;
    Connection conn(db);

    conn.CreateTable("nullable", {
        {"x", LogicalType::INTEGER()},
    });

    DataChunk chunk;
    chunk.Initialize({LogicalType::INTEGER()});
    chunk.SetValue(0, 0, Value::INTEGER(10));
    chunk.SetValue(0, 1, Value());  // NULL
    chunk.SetValue(0, 2, Value::INTEGER(30));
    conn.Append("nullable", chunk);

    conn.Scan("nullable", [&](DataChunk &result) {
        CHECK_FALSE(result.GetValue(0, 0).IsNull());
        CHECK(result.GetValue(0, 1).IsNull());
        CHECK_FALSE(result.GetValue(0, 2).IsNull());
        CHECK(result.GetValue(0, 0).GetValue<int32_t>() == 10);
        CHECK(result.GetValue(0, 2).GetValue<int32_t>() == 30);
    });
}

TEST_CASE("DataTable - multiple row groups") {
    Database db;
    Connection conn(db);

    conn.CreateTable("big", {
        {"id", LogicalType::INTEGER()},
    });

    // Insert more than ROW_GROUP_SIZE rows (122880) to test spanning.
    // We'll insert in chunks of VECTOR_SIZE.
    idx_t total_inserted = 0;
    idx_t target = ROW_GROUP_SIZE + 5000; // spans into second row group

    while (total_inserted < target) {
        DataChunk chunk;
        chunk.Initialize({LogicalType::INTEGER()});
        idx_t batch = std::min(VECTOR_SIZE, target - total_inserted);
        for (idx_t i = 0; i < batch; i++) {
            chunk.SetValue(0, i, Value::INTEGER(static_cast<int32_t>(total_inserted + i)));
        }
        conn.Append("big", chunk);
        total_inserted += batch;
    }

    // Scan and count.
    idx_t scanned = 0;
    conn.Scan("big", [&](DataChunk &result) {
        for (idx_t i = 0; i < result.size(); i++) {
            auto val = result.GetValue(0, i).GetValue<int32_t>();
            CHECK(val == static_cast<int32_t>(scanned + i));
        }
        scanned += result.size();
    });
    CHECK(scanned == target);
}

TEST_CASE("DataTable - drop table") {
    Database db;
    Connection conn(db);

    conn.CreateTable("tmp", {{"x", LogicalType::INTEGER()}});
    conn.DropTable("tmp");
    CHECK_THROWS_AS(conn.DropTable("tmp"), CatalogException);
}

TEST_CASE("DataTable - append to nonexistent table throws") {
    Database db;
    Connection conn(db);

    DataChunk chunk;
    chunk.Initialize({LogicalType::INTEGER()});
    chunk.SetValue(0, 0, Value::INTEGER(1));
    CHECK_THROWS_AS(conn.Append("nope", chunk), CatalogException);
}

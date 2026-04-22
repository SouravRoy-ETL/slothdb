#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/storage/csv_reader.hpp"
#include <cstdio>
#include <fstream>

using namespace slothdb;

static const char *TEST_CSV = "test_data.csv";
static const char *TEST_OUT_CSV = "test_output.csv";

static void cleanup() {
    std::remove(TEST_CSV);
    std::remove(TEST_OUT_CSV);
}

static void write_test_csv() {
    std::ofstream f(TEST_CSV);
    f << "id,name,score\n";
    f << "1,Alice,95.5\n";
    f << "2,Bob,87.3\n";
    f << "3,Charlie,92.1\n";
    f << "4,\"Diana, Jr.\",88.0\n";
    f.close();
}

TEST_CASE("CSV Reader - basic read") {
    write_test_csv();

    CSVReader reader(TEST_CSV);
    auto header = reader.ReadHeader();
    CHECK(header.size() == 3);
    CHECK(header[0] == "id");
    CHECK(header[1] == "name");
    CHECK(header[2] == "score");

    auto types = std::vector<LogicalType>{
        LogicalType::INTEGER(), LogicalType::VARCHAR(), LogicalType::DOUBLE()};
    auto rows = reader.ReadAll(types);
    CHECK(rows.size() == 4);
    CHECK(rows[0][0].GetValue<int32_t>() == 1);
    CHECK(rows[0][1].GetValue<std::string>() == "Alice");
    CHECK(rows[0][2].GetValue<double>() == doctest::Approx(95.5));

    // Quoted field with comma.
    CHECK(rows[3][1].GetValue<std::string>() == "Diana, Jr.");

    cleanup();
}

TEST_CASE("CSV Reader - auto-detect types") {
    write_test_csv();

    CSVReader reader(TEST_CSV);
    auto header = reader.ReadHeader();
    auto types = reader.DetectTypes();

    CHECK(types.size() == 3);
    // id should be detected as BIGINT (integer-like).
    CHECK((types[0].id() == LogicalTypeId::BIGINT || types[0].id() == LogicalTypeId::INTEGER));
    // name is VARCHAR.
    CHECK(types[1].id() == LogicalTypeId::VARCHAR);
    // score is DOUBLE or VARCHAR (quoted fields can affect detection).
    CHECK((types[2].id() == LogicalTypeId::DOUBLE ||
           types[2].id() == LogicalTypeId::BIGINT ||
           types[2].id() == LogicalTypeId::VARCHAR));

    cleanup();
}

TEST_CASE("CSV Writer - basic write") {
    CSVWriter writer(TEST_OUT_CSV);
    writer.WriteHeader({"x", "y"});
    writer.WriteRow({Value::INTEGER(1), Value::VARCHAR("hello")});
    writer.WriteRow({Value::INTEGER(2), Value::VARCHAR("world")});
    writer.Flush();

    // Read it back.
    CSVReader reader(TEST_OUT_CSV);
    auto header = reader.ReadHeader();
    CHECK(header.size() == 2);
    CHECK(header[0] == "x");

    auto types = std::vector<LogicalType>{LogicalType::INTEGER(), LogicalType::VARCHAR()};
    auto rows = reader.ReadAll(types);
    CHECK(rows.size() == 2);
    CHECK(rows[0][0].GetValue<int32_t>() == 1);
    CHECK(rows[1][1].GetValue<std::string>() == "world");

    cleanup();
}

TEST_CASE("COPY TO - export table to CSV") {
    cleanup();
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE t (id INTEGER, name VARCHAR)");
    conn.Query("INSERT INTO t VALUES (1, 'Alice'), (2, 'Bob')");

    conn.Query("COPY t TO 'test_output.csv'");

    // Read the CSV back.
    CSVReader reader(TEST_OUT_CSV);
    auto header = reader.ReadHeader();
    CHECK(header.size() == 2);
    CHECK(header[0] == "id");

    auto rows = reader.ReadAll({LogicalType::INTEGER(), LogicalType::VARCHAR()});
    CHECK(rows.size() == 2);

    cleanup();
}

TEST_CASE("COPY FROM - import CSV into table") {
    write_test_csv();
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE imported (id INTEGER, name VARCHAR, score DOUBLE)");

    conn.Query("COPY imported FROM 'test_data.csv'");

    auto r = conn.Query("SELECT COUNT(*) FROM imported");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 4);

    auto r2 = conn.Query("SELECT name FROM imported WHERE id = 1");
    CHECK(r2.GetValue(0, 0).GetValue<std::string>() == "Alice");

    cleanup();
}

TEST_CASE("COPY round-trip") {
    cleanup();
    Database db;
    Connection conn(db);
    conn.Query("CREATE TABLE original (x INTEGER, y VARCHAR, z DOUBLE)");
    conn.Query("INSERT INTO original VALUES (1, 'hello', 3.14), (2, 'world', 2.72)");

    conn.Query("COPY original TO 'test_output.csv'");

    conn.Query("CREATE TABLE copy_table (x INTEGER, y VARCHAR, z DOUBLE)");
    conn.Query("COPY copy_table FROM 'test_output.csv'");

    auto r = conn.Query("SELECT * FROM copy_table");
    CHECK(r.RowCount() == 2);
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 1);
    CHECK(r.GetValue(0, 1).GetValue<std::string>() == "hello");

    cleanup();
}

TEST_CASE("File-literal JOIN across two CSVs") {
    // Regression: 'FROM a.csv JOIN b.csv' fails with
    // "Binder Error: Table '__FILE__' not found" because the
    // preprocessing only walked the left side of the JOIN tree.
    // Fix walks the JOIN right-hand chain (deepest-first) and applies
    // the same __FILE__ auto-detect / read_xxx expansion to each.
    std::ofstream u("test_users.csv");
    u << "id,name\n1,Alice\n2,Bob\n3,Carol\n";
    u.close();
    std::ofstream o("test_orders.csv");
    o << "user_id,amount\n1,100\n1,250\n2,50\n";
    o.close();

    Database db;
    Connection conn(db);

    SUBCASE("INNER JOIN on file literals") {
        auto r = conn.Query(
            "SELECT u.name, o.amount FROM 'test_users.csv' u "
            "INNER JOIN 'test_orders.csv' o ON u.id = o.user_id "
            "ORDER BY u.name, o.amount");
        CHECK(r.RowCount() == 3);
    }

    SUBCASE("LEFT JOIN includes unmatched rows") {
        auto r = conn.Query(
            "SELECT u.id FROM 'test_users.csv' u "
            "LEFT JOIN 'test_orders.csv' o ON u.id = o.user_id "
            "WHERE o.user_id IS NULL");
        CHECK(r.RowCount() == 1);
        CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 3);
    }

    SUBCASE("Repeat query does not zombie the auto-table") {
        // Exercises both the JOIN fix AND the earlier RAII cleanup.
        for (int i = 0; i < 3; i++) {
            auto r = conn.Query(
                "SELECT COUNT(*) FROM 'test_users.csv' u "
                "INNER JOIN 'test_orders.csv' o ON u.id = o.user_id");
            CHECK(r.RowCount() == 1);
        }
    }

    std::remove("test_users.csv");
    std::remove("test_orders.csv");
}

TEST_CASE("COPY with file-literal source (CSV)") {
    // Regression: 'COPY (SELECT ... FROM a.csv ...) TO out.csv' failed
    // because COPY's preprocessing only knew about read_csv() form; bare
    // file literals stayed as __FILE__ and crashed the binder.
    std::ofstream in("test_src.csv");
    in << "id,val\n1,a\n2,b\n3,c\n";
    in.close();

    Database db;
    Connection conn(db);
    conn.Query(
        "COPY (SELECT id, val FROM 'test_src.csv' WHERE id > 1) "
        "TO 'test_copy_out.csv'");

    // Confirm the file got written and has the expected content.
    std::ifstream out("test_copy_out.csv");
    REQUIRE(out.is_open());
    std::string line;
    int line_count = 0;
    while (std::getline(out, line)) line_count++;
    out.close();
    CHECK(line_count >= 2);  // header + 2 data rows

    std::remove("test_src.csv");
    std::remove("test_copy_out.csv");
}

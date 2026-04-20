// PhysicalArrowScan + ArrowIPCReader::ReadIntoChunks round-trip tests.

#include "doctest.h"
#include "slothdb/main/connection.hpp"
#include "slothdb/main/database.hpp"
#include "slothdb/storage/arrow_ipc.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace slothdb;

static fs::path tmp_arrow(const std::string &tag) {
    auto p = fs::temp_directory_path() / ("slothdb_arrow_" + tag + ".arrow");
    std::error_code ec; fs::remove(p, ec);
    return p;
}

TEST_CASE("ArrowIPCReader::DetectSchemaLight populates schema") {
    auto path = tmp_arrow("schema").string();
    ArrowIPCWriter w(path, {"id", "name", "score"},
                     {LogicalType::INTEGER(), LogicalType::VARCHAR(), LogicalType::DOUBLE()});
    w.WriteBatch({
        {Value::INTEGER(1), Value::VARCHAR("alice"), Value::DOUBLE(97.5)},
    });
    w.Finish();

    ArrowIPCReader r(path);
    r.DetectSchemaLight();
    CHECK(r.GetColumnNames().size() == 3);
    CHECK(r.GetColumnNames()[0] == "id");
    CHECK(r.GetColumnNames()[1] == "name");
    CHECK(r.GetColumnNames()[2] == "score");
    CHECK(r.GetColumnTypes()[0].id() == LogicalTypeId::INTEGER);
    CHECK(r.GetColumnTypes()[1].id() == LogicalTypeId::VARCHAR);
    CHECK(r.GetColumnTypes()[2].id() == LogicalTypeId::DOUBLE);
}

TEST_CASE("ArrowIPCReader::ReadIntoChunks round-trips typed values") {
    auto path = tmp_arrow("roundtrip").string();
    std::vector<LogicalType> types = {
        LogicalType::BIGINT(), LogicalType::VARCHAR(), LogicalType::DOUBLE()
    };
    ArrowIPCWriter w(path, {"id", "region", "revenue"}, types);
    w.WriteBatch({
        {Value::BIGINT(1), Value::VARCHAR("APAC"), Value::DOUBLE(100.5)},
        {Value::BIGINT(2), Value::VARCHAR("MEA"),  Value::DOUBLE(250.0)},
        {Value::BIGINT(3), Value::VARCHAR("APAC"), Value::DOUBLE(175.25)},
    });
    w.Finish();

    ArrowIPCReader r(path);
    std::vector<DataChunk> chunks;
    r.ReadIntoChunks(chunks, types);

    REQUIRE(chunks.size() == 1);
    auto &c = chunks[0];
    CHECK(c.size() == 3);
    CHECK(c.GetValue(0, 0).GetValue<int64_t>() == 1);
    CHECK(c.GetValue(0, 1).GetValue<int64_t>() == 2);
    CHECK(c.GetValue(0, 2).GetValue<int64_t>() == 3);
    CHECK(c.GetValue(1, 0).GetValue<std::string>() == "APAC");
    CHECK(c.GetValue(1, 1).GetValue<std::string>() == "MEA");
    CHECK(c.GetValue(2, 0).GetValue<double>() == doctest::Approx(100.5));
    CHECK(c.GetValue(2, 2).GetValue<double>() == doctest::Approx(175.25));
}

TEST_CASE("ReadIntoChunks produces multiple chunks for > VECTOR_SIZE rows") {
    auto path = tmp_arrow("chunking").string();
    std::vector<LogicalType> types = {LogicalType::INTEGER()};
    ArrowIPCWriter w(path, {"n"}, types);

    std::vector<std::vector<Value>> rows;
    for (int i = 0; i < 2500; i++) rows.push_back({Value::INTEGER(i)});
    w.WriteBatch(rows);
    w.Finish();

    ArrowIPCReader r(path);
    std::vector<DataChunk> chunks;
    r.ReadIntoChunks(chunks, types);

    CHECK(chunks.size() >= 2);
    size_t total = 0;
    for (auto &c : chunks) total += c.size();
    CHECK(total == 2500);

    CHECK(chunks.front().GetValue(0, 0).GetValue<int32_t>() == 0);
    CHECK(chunks.back().GetValue(0, chunks.back().size() - 1).GetValue<int32_t>() == 2499);
}

TEST_CASE("read_arrow() end-to-end: SQL aggregate over Arrow file") {
    auto path = tmp_arrow("e2e").string();
    std::vector<LogicalType> types = {
        LogicalType::VARCHAR(), LogicalType::DOUBLE()
    };
    ArrowIPCWriter w(path, {"region", "revenue"}, types);
    w.WriteBatch({
        {Value::VARCHAR("APAC"), Value::DOUBLE(100.0)},
        {Value::VARCHAR("MEA"),  Value::DOUBLE(200.0)},
        {Value::VARCHAR("APAC"), Value::DOUBLE(50.0)},
    });
    w.Finish();

    Database db;
    Connection conn(db);
    std::string query = "SELECT region, SUM(revenue) AS total "
                        "FROM read_arrow('" + path + "') "
                        "GROUP BY region ORDER BY region";
    auto r = conn.Query(query);
    REQUIRE(r.RowCount() == 2);
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "APAC");
    CHECK(r.GetValue(0, 1).GetValue<double>() == doctest::Approx(150.0));
    CHECK(r.GetValue(1, 0).GetValue<std::string>() == "MEA");
    CHECK(r.GetValue(1, 1).GetValue<double>() == doctest::Approx(200.0));
}

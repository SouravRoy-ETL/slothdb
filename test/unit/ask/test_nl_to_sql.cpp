#include "doctest.h"
#include "slothdb/ask/nl_to_sql.hpp"

using namespace slothdb::ask;

namespace {
Schema SalesSchema() {
    Schema s;
    s.tables.push_back({
        "sales",
        {
            {"id", "INTEGER"},
            {"customer_id", "INTEGER"},
            {"amount", "DOUBLE"},
            {"order_date", "VARCHAR"},
            {"region", "VARCHAR"},
        }
    });
    return s;
}

Schema TwoTableSchema() {
    Schema s = SalesSchema();
    s.tables.push_back({
        "products",
        {
            {"product_id", "INTEGER"},
            {"name", "VARCHAR"},
            {"price", "DOUBLE"},
        }
    });
    return s;
}
} // namespace

// ---------------------------------------------------------------------------
// COUNT
// ---------------------------------------------------------------------------

TEST_CASE("ask: COUNT — 'how many sales'") {
    auto r = Translate("how many sales", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("SELECT COUNT(*) FROM \"sales\"") != std::string::npos);
}

TEST_CASE("ask: COUNT — 'count of sales'") {
    auto r = Translate("count of sales", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("COUNT(*) FROM \"sales\"") != std::string::npos);
}

TEST_CASE("ask: COUNT — 'number of sales'") {
    auto r = Translate("number of sales", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("COUNT(*)") != std::string::npos);
}

TEST_CASE("ask: COUNT — plural/singular matching") {
    auto r = Translate("how many sale", SalesSchema());
    CHECK(r.status == Status::OK); // "sale" resolves to "sales"
}

TEST_CASE("ask: COUNT — with year filter 'in 2024'") {
    auto r = Translate("how many sales in 2024", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("WHERE") != std::string::npos);
    CHECK(r.sql.find("2024") != std::string::npos);
}

// ---------------------------------------------------------------------------
// SUM / AVG / MIN / MAX
// ---------------------------------------------------------------------------

TEST_CASE("ask: SUM — 'total amount'") {
    auto r = Translate("total amount", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("SUM(\"amount\")") != std::string::npos);
}

TEST_CASE("ask: SUM — 'sum of amount'") {
    auto r = Translate("sum of amount", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("SUM(\"amount\")") != std::string::npos);
}

TEST_CASE("ask: SUM — 'total amount per region' groups") {
    auto r = Translate("total amount per region", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("SUM(\"amount\")") != std::string::npos);
    CHECK(r.sql.find("GROUP BY \"region\"") != std::string::npos);
}

TEST_CASE("ask: AVG — 'average amount'") {
    auto r = Translate("average amount", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("AVG(\"amount\")") != std::string::npos);
}

TEST_CASE("ask: MIN — 'min amount'") {
    auto r = Translate("min amount", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("MIN(\"amount\")") != std::string::npos);
}

TEST_CASE("ask: MAX — 'maximum amount'") {
    auto r = Translate("maximum amount", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("MAX(\"amount\")") != std::string::npos);
}

TEST_CASE("ask: SUM + year filter — 'total amount in 2024'") {
    auto r = Translate("total amount in 2024", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("SUM(\"amount\")") != std::string::npos);
    CHECK(r.sql.find("2024") != std::string::npos);
}

// ---------------------------------------------------------------------------
// TOP N
// ---------------------------------------------------------------------------

TEST_CASE("ask: TOP N — 'top 5 customer_id by amount'") {
    auto r = Translate("top 5 customer_id by amount", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("LIMIT 5") != std::string::npos);
    CHECK(r.sql.find("ORDER BY") != std::string::npos);
    CHECK(r.sql.find("DESC") != std::string::npos);
}

TEST_CASE("ask: TOP N — 'bottom 3 region by amount' sorts ASC") {
    auto r = Translate("bottom 3 region by amount", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("LIMIT 3") != std::string::npos);
    CHECK(r.sql.find("ASC") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Synonyms
// ---------------------------------------------------------------------------

TEST_CASE("ask: synonym — 'total revenue' resolves to amount column") {
    // 'revenue' has no column; synonyms should route to 'amount'/'total'.
    auto r = Translate("total revenue", SalesSchema());
    CHECK(r.status == Status::OK);
    // Either amount or id — but must be a numeric column.
    CHECK((r.sql.find("SUM(\"amount\")") != std::string::npos ||
           r.sql.find("SUM(\"total\")") != std::string::npos));
}

// ---------------------------------------------------------------------------
// SELECT *
// ---------------------------------------------------------------------------

TEST_CASE("ask: bare — 'rows from sales'") {
    auto r = Translate("rows from sales", SalesSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("SELECT * FROM \"sales\"") != std::string::npos);
    CHECK(r.sql.find("LIMIT 100") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Failure modes
// ---------------------------------------------------------------------------

TEST_CASE("ask: unresolved table") {
    auto r = Translate("how many orders", SalesSchema());
    CHECK(r.status == Status::UNRESOLVED_TABLE);
    CHECK(r.unresolved == "orders");
}

TEST_CASE("ask: unresolved column (as table)") {
    auto r = Translate("total mangoes per region", SalesSchema());
    CHECK(r.status != Status::OK);
}

TEST_CASE("ask: empty question") {
    auto r = Translate("", SalesSchema());
    CHECK(r.status == Status::NO_MATCH);
}

TEST_CASE("ask: open-ended question gets a helpful no-match") {
    auto r = Translate("which month had the most loyal repeat buyers",
                       SalesSchema());
    CHECK(r.status != Status::OK);
    CHECK(!r.message.empty());
}

// ---------------------------------------------------------------------------
// Two-table schema — "implicit table via column match"
// ---------------------------------------------------------------------------

TEST_CASE("ask: agg picks the table whose columns match") {
    // 'price' only exists in products — SUM should route there.
    auto r = Translate("total price", TwoTableSchema());
    CHECK(r.status == Status::OK);
    CHECK(r.sql.find("\"products\"") != std::string::npos);
    CHECK(r.sql.find("SUM(\"price\")") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST_CASE("ask: same input produces same output") {
    auto s = SalesSchema();
    auto r1 = Translate("total amount per region", s);
    auto r2 = Translate("total amount per region", s);
    CHECK(r1.sql == r2.sql);
    CHECK(r1.status == r2.status);
}

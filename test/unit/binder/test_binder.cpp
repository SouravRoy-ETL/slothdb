#include "doctest.h"
#include "slothdb/binder/binder.hpp"
#include "slothdb/parser/parser.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/storage/data_table.hpp"

using namespace slothdb;

// Helper: create a catalog with a test table.
static std::unique_ptr<Catalog> make_catalog() {
    auto catalog = std::make_unique<Catalog>();
    auto &entry = catalog->CreateTable("users", {
        {"id", LogicalType::INTEGER()},
        {"name", LogicalType::VARCHAR()},
        {"score", LogicalType::DOUBLE()},
    });
    auto types = entry.GetTypes();
    entry.SetStorage(std::make_shared<DataTable>(types));
    return catalog;
}

TEST_CASE("Binder - SELECT *") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT * FROM users");
    auto bound = binder.Bind(*stmts[0]);

    REQUIRE(bound->GetType() == BoundStatementType::SELECT);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);

    CHECK(sel.select_list.size() == 3);
    CHECK(sel.result_names[0] == "id");
    CHECK(sel.result_names[1] == "name");
    CHECK(sel.result_names[2] == "score");
    CHECK(sel.result_types[0].id() == LogicalTypeId::INTEGER);
    CHECK(sel.result_types[1].id() == LogicalTypeId::VARCHAR);
    CHECK(sel.result_types[2].id() == LogicalTypeId::DOUBLE);
}

TEST_CASE("Binder - SELECT * with ORDER BY named column") {
    // Regression: 'SELECT * FROM t ORDER BY name' was throwing
    // "Internal Error: Unhandled expression type in binder" because
    // the alias-resolution fast-path re-bound select_list[0] - which
    // is an unbound STAR expression. Guard added: skip alias path if
    // select_list[i] is a STAR, fall through to generic name lookup.
    auto catalog = make_catalog();
    Binder binder(*catalog);

    auto stmts = Parser::Parse("SELECT * FROM users ORDER BY name");
    auto bound = binder.Bind(*stmts[0]);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);
    CHECK(sel.order_by.size() == 1);

    auto stmts2 = Parser::Parse("SELECT * FROM users ORDER BY score DESC");
    auto bound2 = binder.Bind(*stmts2[0]);
    auto &sel2 = static_cast<BoundSelectStatement &>(*bound2);
    CHECK(sel2.order_by.size() == 1);
    CHECK(sel2.order_by[0].ascending == false);

    // Positional ORDER BY, which was already working, should still work.
    auto stmts3 = Parser::Parse("SELECT * FROM users ORDER BY 1");
    auto bound3 = binder.Bind(*stmts3[0]);
    CHECK(static_cast<BoundSelectStatement &>(*bound3).order_by.size() == 1);

    // Alias-to-select-list resolution (non-star) must still work.
    auto stmts4 = Parser::Parse("SELECT id + 1 AS bumped FROM users ORDER BY bumped");
    auto bound4 = binder.Bind(*stmts4[0]);
    CHECK(static_cast<BoundSelectStatement &>(*bound4).order_by.size() == 1);
}

TEST_CASE("Binder - SELECT specific columns") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT id, score FROM users");
    auto bound = binder.Bind(*stmts[0]);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);

    CHECK(sel.select_list.size() == 2);
    CHECK(sel.result_types[0].id() == LogicalTypeId::INTEGER);
    CHECK(sel.result_types[1].id() == LogicalTypeId::DOUBLE);

    auto &col0 = static_cast<BoundColumnRef &>(*sel.select_list[0]);
    CHECK(col0.column_index == 0);
    auto &col1 = static_cast<BoundColumnRef &>(*sel.select_list[1]);
    CHECK(col1.column_index == 2);
}

TEST_CASE("Binder - WHERE clause type resolution") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT * FROM users WHERE score > 90.0");
    auto bound = binder.Bind(*stmts[0]);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);

    REQUIRE(sel.where_clause != nullptr);
    CHECK(sel.where_clause->GetReturnType().id() == LogicalTypeId::BOOLEAN);
}

TEST_CASE("Binder - arithmetic type promotion") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT id + score FROM users");
    auto bound = binder.Bind(*stmts[0]);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);

    // INTEGER + DOUBLE -> DOUBLE
    CHECK(sel.result_types[0].id() == LogicalTypeId::DOUBLE);
}

TEST_CASE("Binder - aggregate function return types") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT COUNT(*), SUM(id), AVG(score), MIN(name) FROM users");
    auto bound = binder.Bind(*stmts[0]);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);

    CHECK(sel.has_aggregation == true);
    CHECK(sel.result_types[0].id() == LogicalTypeId::BIGINT);  // COUNT
    CHECK(sel.result_types[1].id() == LogicalTypeId::BIGINT);  // SUM(int) -> BIGINT
    CHECK(sel.result_types[2].id() == LogicalTypeId::DOUBLE);  // AVG
    CHECK(sel.result_types[3].id() == LogicalTypeId::VARCHAR);  // MIN(varchar)
}

TEST_CASE("Binder - table not found throws structured error") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT * FROM nonexistent");

    try {
        binder.Bind(*stmts[0]);
        CHECK(false);
    } catch (const BinderException &e) {
        CHECK(e.GetCategory() == ErrorCategory::BINDER);
        CHECK(e.GetCode() == ErrorCode::TABLE_NOT_FOUND);
    }
}

TEST_CASE("Binder - column not found throws structured error") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT xyz FROM users");

    try {
        binder.Bind(*stmts[0]);
        CHECK(false);
    } catch (const BinderException &e) {
        CHECK(e.GetCode() == ErrorCode::COLUMN_NOT_FOUND);
    }
}

TEST_CASE("Binder - CREATE TABLE") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("CREATE TABLE t2 (a INTEGER, b VARCHAR, c DOUBLE)");
    auto bound = binder.Bind(*stmts[0]);

    REQUIRE(bound->GetType() == BoundStatementType::CREATE_TABLE);
    auto &ct = static_cast<BoundCreateTableStatement &>(*bound);
    CHECK(ct.table_name == "t2");
    CHECK(ct.columns.size() == 3);
    CHECK(ct.columns[0].type.id() == LogicalTypeId::INTEGER);
    CHECK(ct.columns[1].type.id() == LogicalTypeId::VARCHAR);
    CHECK(ct.columns[2].type.id() == LogicalTypeId::DOUBLE);
}

TEST_CASE("Binder - INSERT INTO") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("INSERT INTO users VALUES (1, 'Alice', 95.5)");
    auto bound = binder.Bind(*stmts[0]);

    REQUIRE(bound->GetType() == BoundStatementType::INSERT);
    auto &ins = static_cast<BoundInsertStatement &>(*bound);
    CHECK(ins.table != nullptr);
    CHECK(ins.values.size() == 1);
    CHECK(ins.values[0].size() == 3);
}

TEST_CASE("Binder - INSERT into nonexistent table") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("INSERT INTO nope VALUES (1)");

    CHECK_THROWS_AS(binder.Bind(*stmts[0]), BinderException);
}

TEST_CASE("Binder - DROP TABLE") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("DROP TABLE users");
    auto bound = binder.Bind(*stmts[0]);
    CHECK(bound->GetType() == BoundStatementType::DROP_TABLE);
}

TEST_CASE("Binder - DROP nonexistent table throws") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("DROP TABLE nope");
    CHECK_THROWS_AS(binder.Bind(*stmts[0]), BinderException);
}

TEST_CASE("Binder - DROP IF EXISTS nonexistent table ok") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("DROP TABLE IF EXISTS nope");
    auto bound = binder.Bind(*stmts[0]); // should not throw
    CHECK(bound->GetType() == BoundStatementType::DROP_TABLE);
}

TEST_CASE("Binder - constant expression") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT 42, 'hello', 3.14, TRUE, NULL FROM users");
    auto bound = binder.Bind(*stmts[0]);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);

    CHECK(sel.result_types[0].id() == LogicalTypeId::INTEGER);
    CHECK(sel.result_types[1].id() == LogicalTypeId::VARCHAR);
    CHECK(sel.result_types[2].id() == LogicalTypeId::DOUBLE);
    CHECK(sel.result_types[3].id() == LogicalTypeId::BOOLEAN);
    CHECK(sel.result_types[4].id() == LogicalTypeId::SQLNULL);
}

TEST_CASE("Binder - LIMIT and OFFSET") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT * FROM users LIMIT 10 OFFSET 5");
    auto bound = binder.Bind(*stmts[0]);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);

    CHECK(sel.limit_count == 10);
    CHECK(sel.offset_count == 5);
}

TEST_CASE("Binder - alias in select") {
    auto catalog = make_catalog();
    Binder binder(*catalog);
    auto stmts = Parser::Parse("SELECT id AS user_id FROM users");
    auto bound = binder.Bind(*stmts[0]);
    auto &sel = static_cast<BoundSelectStatement &>(*bound);

    CHECK(sel.result_names[0] == "user_id");
}

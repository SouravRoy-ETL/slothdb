#include "doctest.h"
#include "slothdb/parser/parser.hpp"
#include "slothdb/common/exception.hpp"

using namespace slothdb;

TEST_CASE("Parser - simple SELECT *") {
    auto stmts = Parser::Parse("SELECT * FROM t1");
    REQUIRE(stmts.size() == 1);
    REQUIRE(stmts[0]->GetType() == StatementType::SELECT);

    auto &sel = static_cast<SelectStatement &>(*stmts[0]);
    CHECK(sel.select_list.size() == 1);
    CHECK(sel.select_list[0]->GetExpressionType() == ExpressionType::STAR);
    CHECK(sel.from_table->table_name == "t1");
    CHECK(sel.where_clause == nullptr);
}

TEST_CASE("Parser - SELECT with WHERE") {
    auto stmts = Parser::Parse("SELECT a, b FROM t1 WHERE a > 10");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);

    CHECK(sel.select_list.size() == 2);
    CHECK(sel.where_clause != nullptr);
    CHECK(sel.where_clause->GetExpressionType() == ExpressionType::COMPARISON);

    auto &cmp = static_cast<ComparisonExpression &>(*sel.where_clause);
    CHECK(cmp.op == ">");
}

TEST_CASE("Parser - SELECT with ORDER BY and LIMIT") {
    auto stmts = Parser::Parse("SELECT x FROM t ORDER BY x DESC LIMIT 10 OFFSET 5");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);

    CHECK(sel.order_by.size() == 1);
    CHECK(sel.order_by[0].ascending == false);
    CHECK(sel.limit != nullptr);
    CHECK(sel.offset != nullptr);
}

TEST_CASE("Parser - SELECT with GROUP BY and HAVING") {
    auto stmts = Parser::Parse("SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 5");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);

    CHECK(sel.group_by.size() == 1);
    CHECK(sel.having_clause != nullptr);
    CHECK(sel.select_list.size() == 2);

    // COUNT(*) should be a FunctionExpression.
    CHECK(sel.select_list[1]->GetExpressionType() == ExpressionType::FUNCTION);
    auto &fn = static_cast<FunctionExpression &>(*sel.select_list[1]);
    CHECK(fn.function_name == "COUNT");
}

TEST_CASE("Parser - SELECT with alias") {
    auto stmts = Parser::Parse("SELECT a AS col_a, b col_b FROM t");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);

    CHECK(sel.select_list[0]->alias == "col_a");
    CHECK(sel.select_list[1]->alias == "col_b");
}

TEST_CASE("Parser - non-reserved keywords as aliases") {
    // Time-unit keywords (MONTH, DAY, YEAR, HOUR, MINUTE, SECOND, EPOCH, DOW)
    // tokenize as keywords for DATE_TRUNC / EXTRACT but must remain legal as
    // column aliases and table-alias identifiers. Regression for a bug where
    // `SELECT MONTHNAME(x) AS month` threw "Expected IDENTIFIER after AS".
    {
        auto stmts = Parser::Parse("SELECT MONTHNAME(x) AS month FROM t");
        auto &sel = static_cast<SelectStatement &>(*stmts[0]);
        CHECK(sel.select_list[0]->alias == "month");
    }
    {
        auto stmts = Parser::Parse("SELECT DAYNAME(x) AS day, MONTHNAME(x) AS month, x AS year FROM t");
        auto &sel = static_cast<SelectStatement &>(*stmts[0]);
        CHECK(sel.select_list[0]->alias == "day");
        CHECK(sel.select_list[1]->alias == "month");
        CHECK(sel.select_list[2]->alias == "year");
    }
    {
        // Bare alias (no AS) must also accept non-reserved keywords.
        auto stmts = Parser::Parse("SELECT MONTHNAME(x) month FROM t");
        auto &sel = static_cast<SelectStatement &>(*stmts[0]);
        CHECK(sel.select_list[0]->alias == "month");
    }
    {
        // Table alias in FROM.
        auto stmts = Parser::Parse("SELECT x FROM t AS month");
        auto &sel = static_cast<SelectStatement &>(*stmts[0]);
        CHECK(sel.from_table->alias == "month");
    }
    {
        // Window-frame keywords (ROWS, RANGE, ROW, UNBOUNDED, PRECEDING,
        // FOLLOWING, CURRENT) must also be non-reserved. User-reported:
        // SELECT COUNT(*) AS rows FROM t.
        auto stmts = Parser::Parse("SELECT COUNT(*) AS rows FROM t");
        auto &sel = static_cast<SelectStatement &>(*stmts[0]);
        CHECK(sel.select_list[0]->alias == "rows");
    }
    {
        // Aggregate function names (COUNT, SUM, AVG, MIN, MAX) as aliases.
        auto stmts = Parser::Parse("SELECT x AS count, y AS sum, z AS avg FROM t");
        auto &sel = static_cast<SelectStatement &>(*stmts[0]);
        CHECK(sel.select_list[0]->alias == "count");
        CHECK(sel.select_list[1]->alias == "sum");
        CHECK(sel.select_list[2]->alias == "avg");
    }
    {
        // Reserved keywords (WHERE, GROUP, JOIN, etc.) must still be
        // rejected as aliases — regression guard so the non-reserved set
        // doesn't swallow clause terminators.
        CHECK_THROWS_AS(Parser::Parse("SELECT x AS where FROM t"), ParserException);
        CHECK_THROWS_AS(Parser::Parse("SELECT x AS join FROM t"),  ParserException);
        CHECK_THROWS_AS(Parser::Parse("SELECT x AS qualify FROM t"), ParserException);
    }
}

TEST_CASE("Parser - SELECT DISTINCT") {
    auto stmts = Parser::Parse("SELECT DISTINCT x FROM t");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);
    CHECK(sel.is_distinct == true);
}

TEST_CASE("Parser - SELECT with JOIN") {
    auto stmts = Parser::Parse("SELECT * FROM t1 INNER JOIN t2 ON t1.id = t2.id");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);

    CHECK(sel.from_table->join_type == "INNER");
    CHECK(sel.from_table->right != nullptr);
    CHECK(sel.from_table->right->table_name == "t2");
    CHECK(sel.from_table->on_condition != nullptr);
}

TEST_CASE("Parser - SELECT with LEFT JOIN") {
    auto stmts = Parser::Parse("SELECT * FROM t1 LEFT JOIN t2 ON t1.id = t2.id");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);
    CHECK(sel.from_table->join_type == "LEFT");
}

TEST_CASE("Parser - table.column reference") {
    auto stmts = Parser::Parse("SELECT t1.a FROM t1");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);

    CHECK(sel.select_list[0]->GetExpressionType() == ExpressionType::COLUMN_REF);
    auto &col = static_cast<ColumnRefExpression &>(*sel.select_list[0]);
    CHECK(col.table_name == "t1");
    CHECK(col.column_name == "a");
}

TEST_CASE("Parser - arithmetic expressions") {
    auto stmts = Parser::Parse("SELECT a + b * 2 FROM t");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);

    // Should be: a + (b * 2) due to precedence.
    auto &add = static_cast<ArithmeticExpression &>(*sel.select_list[0]);
    CHECK(add.op == "+");
    CHECK(add.right->GetExpressionType() == ExpressionType::ARITHMETIC);
}

TEST_CASE("Parser - IS NULL") {
    auto stmts = Parser::Parse("SELECT * FROM t WHERE x IS NULL");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);
    CHECK(sel.where_clause->GetExpressionType() == ExpressionType::IS_NULL);
    auto &isn = static_cast<IsNullExpression &>(*sel.where_clause);
    CHECK(isn.is_not == false);
}

TEST_CASE("Parser - IS NOT NULL") {
    auto stmts = Parser::Parse("SELECT * FROM t WHERE x IS NOT NULL");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);
    auto &isn = static_cast<IsNullExpression &>(*sel.where_clause);
    CHECK(isn.is_not == true);
}

TEST_CASE("Parser - CREATE TABLE") {
    auto stmts = Parser::Parse(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, score DOUBLE)");
    REQUIRE(stmts.size() == 1);
    REQUIRE(stmts[0]->GetType() == StatementType::CREATE_TABLE);

    auto &ct = static_cast<CreateTableStatement &>(*stmts[0]);
    CHECK(ct.table_name == "users");
    CHECK(ct.columns.size() == 3);
    CHECK(ct.columns[0].name == "id");
    CHECK(ct.columns[0].type_name == "INTEGER");
    CHECK(ct.columns[0].is_primary_key == true);
    CHECK(ct.columns[0].not_null == true);
    CHECK(ct.columns[1].name == "name");
    CHECK(ct.columns[1].type_name == "VARCHAR");
    CHECK(ct.columns[1].not_null == true);
    CHECK(ct.columns[2].name == "score");
    CHECK(ct.columns[2].type_name == "DOUBLE");
    CHECK(ct.columns[2].not_null == false);
}

TEST_CASE("Parser - CREATE TABLE IF NOT EXISTS") {
    auto stmts = Parser::Parse("CREATE TABLE IF NOT EXISTS t (x INT)");
    auto &ct = static_cast<CreateTableStatement &>(*stmts[0]);
    CHECK(ct.if_not_exists == true);
}

TEST_CASE("Parser - CREATE TABLE with DECIMAL") {
    auto stmts = Parser::Parse("CREATE TABLE t (price DECIMAL(10,2))");
    auto &ct = static_cast<CreateTableStatement &>(*stmts[0]);
    CHECK(ct.columns[0].type_name == "DECIMAL(10,2)");
}

TEST_CASE("Parser - DROP TABLE") {
    auto stmts = Parser::Parse("DROP TABLE users");
    REQUIRE(stmts[0]->GetType() == StatementType::DROP_TABLE);
    auto &dt = static_cast<DropTableStatement &>(*stmts[0]);
    CHECK(dt.table_name == "users");
    CHECK(dt.if_exists == false);
}

TEST_CASE("Parser - DROP TABLE IF EXISTS") {
    auto stmts = Parser::Parse("DROP TABLE IF EXISTS users");
    auto &dt = static_cast<DropTableStatement &>(*stmts[0]);
    CHECK(dt.if_exists == true);
}

TEST_CASE("Parser - INSERT INTO VALUES") {
    auto stmts = Parser::Parse("INSERT INTO t1 VALUES (1, 'hello', 3.14)");
    REQUIRE(stmts[0]->GetType() == StatementType::INSERT);

    auto &ins = static_cast<InsertStatement &>(*stmts[0]);
    CHECK(ins.table_name == "t1");
    CHECK(ins.column_names.empty());
    CHECK(ins.values.size() == 1);
    CHECK(ins.values[0].size() == 3);
}

TEST_CASE("Parser - INSERT with column names") {
    auto stmts = Parser::Parse("INSERT INTO t1 (a, b) VALUES (1, 2)");
    auto &ins = static_cast<InsertStatement &>(*stmts[0]);
    CHECK(ins.column_names.size() == 2);
    CHECK(ins.column_names[0] == "a");
    CHECK(ins.column_names[1] == "b");
}

TEST_CASE("Parser - INSERT multiple rows") {
    auto stmts = Parser::Parse("INSERT INTO t VALUES (1), (2), (3)");
    auto &ins = static_cast<InsertStatement &>(*stmts[0]);
    CHECK(ins.values.size() == 3);
}

TEST_CASE("Parser - multiple statements") {
    auto stmts = Parser::Parse("SELECT 1; SELECT 2;");
    CHECK(stmts.size() == 2);
}

TEST_CASE("Parser - CAST expression") {
    auto stmts = Parser::Parse("SELECT CAST(x AS INTEGER) FROM t");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);
    CHECK(sel.select_list[0]->GetExpressionType() == ExpressionType::CAST);
}

TEST_CASE("Parser - nested AND/OR") {
    auto stmts = Parser::Parse("SELECT * FROM t WHERE a > 1 AND b < 2 OR c = 3");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);
    // OR has lower precedence, so: (a > 1 AND b < 2) OR (c = 3)
    CHECK(sel.where_clause->GetExpressionType() == ExpressionType::CONJUNCTION);
    auto &conj = static_cast<ConjunctionExpression &>(*sel.where_clause);
    CHECK(conj.op == "OR");
    CHECK(conj.left->GetExpressionType() == ExpressionType::CONJUNCTION);
}

TEST_CASE("Parser - syntax error gives structured error") {
    try {
        Parser::Parse("SELECT FROM");
        CHECK(false);
    } catch (const ParserException &e) {
        CHECK(e.GetCategory() == ErrorCategory::PARSER);
        CHECK(e.GetNumericCode() >= 1000);
        CHECK(e.GetLine() > 0);
    }
}

TEST_CASE("Parser - function with DISTINCT") {
    auto stmts = Parser::Parse("SELECT COUNT(DISTINCT x) FROM t");
    auto &sel = static_cast<SelectStatement &>(*stmts[0]);
    auto &fn = static_cast<FunctionExpression &>(*sel.select_list[0]);
    CHECK(fn.function_name == "COUNT");
    CHECK(fn.is_distinct == true);
}

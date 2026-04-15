#include "doctest.h"
#include "slothdb/parser/tokenizer.hpp"
#include "slothdb/common/exception.hpp"

using namespace slothdb;

TEST_CASE("Tokenizer - basic SELECT") {
    Tokenizer t("SELECT a, b FROM t1");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].type == TokenType::KW_SELECT);
    CHECK(tokens[1].type == TokenType::IDENTIFIER);
    CHECK(tokens[1].value == "a");
    CHECK(tokens[2].type == TokenType::COMMA);
    CHECK(tokens[3].type == TokenType::IDENTIFIER);
    CHECK(tokens[3].value == "b");
    CHECK(tokens[4].type == TokenType::KW_FROM);
    CHECK(tokens[5].type == TokenType::IDENTIFIER);
    CHECK(tokens[5].value == "t1");
    CHECK(tokens[6].type == TokenType::END_OF_FILE);
}

TEST_CASE("Tokenizer - numbers") {
    Tokenizer t("42 3.14 1e10 2.5e-3");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].type == TokenType::INTEGER_LITERAL);
    CHECK(tokens[0].value == "42");
    CHECK(tokens[1].type == TokenType::FLOAT_LITERAL);
    CHECK(tokens[1].value == "3.14");
    CHECK(tokens[2].type == TokenType::FLOAT_LITERAL);
    CHECK(tokens[2].value == "1e10");
    CHECK(tokens[3].type == TokenType::FLOAT_LITERAL);
    CHECK(tokens[3].value == "2.5e-3");
}

TEST_CASE("Tokenizer - strings") {
    Tokenizer t("'hello' 'it''s'");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].type == TokenType::STRING_LITERAL);
    CHECK(tokens[0].value == "hello");
    CHECK(tokens[1].type == TokenType::STRING_LITERAL);
    CHECK(tokens[1].value == "it's"); // escaped quote
}

TEST_CASE("Tokenizer - operators") {
    Tokenizer t("= != <> < > <= >= + - * / %");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].type == TokenType::EQUALS);
    CHECK(tokens[1].type == TokenType::NOT_EQUALS);
    CHECK(tokens[2].type == TokenType::NOT_EQUALS);
    CHECK(tokens[3].type == TokenType::LESS_THAN);
    CHECK(tokens[4].type == TokenType::GREATER_THAN);
    CHECK(tokens[5].type == TokenType::LESS_EQUALS);
    CHECK(tokens[6].type == TokenType::GREATER_EQUALS);
    CHECK(tokens[7].type == TokenType::PLUS);
    CHECK(tokens[8].type == TokenType::MINUS);
    CHECK(tokens[9].type == TokenType::STAR);
    CHECK(tokens[10].type == TokenType::SLASH);
    CHECK(tokens[11].type == TokenType::PERCENT);
}

TEST_CASE("Tokenizer - case insensitive keywords") {
    Tokenizer t("select FROM Where");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].type == TokenType::KW_SELECT);
    CHECK(tokens[1].type == TokenType::KW_FROM);
    CHECK(tokens[2].type == TokenType::KW_WHERE);
}

TEST_CASE("Tokenizer - line comments") {
    Tokenizer t("SELECT -- this is a comment\n42");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].type == TokenType::KW_SELECT);
    CHECK(tokens[1].type == TokenType::INTEGER_LITERAL);
    CHECK(tokens[1].value == "42");
}

TEST_CASE("Tokenizer - block comments") {
    Tokenizer t("SELECT /* skip this */ 42");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].type == TokenType::KW_SELECT);
    CHECK(tokens[1].type == TokenType::INTEGER_LITERAL);
}

TEST_CASE("Tokenizer - quoted identifiers") {
    Tokenizer t("SELECT \"My Column\" FROM t");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].type == TokenType::KW_SELECT);
    CHECK(tokens[1].type == TokenType::IDENTIFIER);
    CHECK(tokens[1].value == "My Column");
}

TEST_CASE("Tokenizer - line and column tracking") {
    Tokenizer t("SELECT\n  42");
    auto tokens = t.Tokenize();

    CHECK(tokens[0].line == 1);
    CHECK(tokens[1].line == 2);
}

TEST_CASE("Tokenizer - unterminated string throws structured error") {
    Tokenizer t("'hello");
    try {
        t.Tokenize();
        CHECK(false); // should not reach here
    } catch (const ParserException &e) {
        CHECK(e.GetCategory() == ErrorCategory::PARSER);
        CHECK(e.GetCode() == ErrorCode::UNTERMINATED_STRING);
        CHECK(e.GetLine() > 0);
    }
}

TEST_CASE("Tokenizer - unexpected character throws structured error") {
    Tokenizer t("SELECT @");
    try {
        t.Tokenize();
        CHECK(false);
    } catch (const ParserException &e) {
        CHECK(e.GetCode() == ErrorCode::UNEXPECTED_TOKEN);
    }
}

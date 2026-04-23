#pragma once

#include "slothdb/parser/tokenizer.hpp"
#include "slothdb/parser/statement/parsed_statement.hpp"
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

// Recursive-descent SQL parser. No external dependencies.
class Parser {
public:
    // Parse a SQL string into a list of statements.
    static std::vector<ParsedStmtPtr> Parse(const std::string &sql);

private:
    explicit Parser(std::vector<Token> tokens);

    // Statement parsers.
    ParsedStmtPtr ParseStatement();
    ParsedStmtPtr ParseSelectStatement();
    ParsedStmtPtr ParseCreateStatement();
    ParsedStmtPtr ParseDropStatement();
    ParsedStmtPtr ParseInsertStatement();
    ParsedStmtPtr ParseUpdateStatement();
    ParsedStmtPtr ParseDeleteStatement();
    ParsedStmtPtr ParseExplainStatement();
    ParsedStmtPtr ParseDescribeStatement();
    ParsedStmtPtr ParsePragmaStatement();
    ParsedStmtPtr ParseMergeStatement();

    // Expression parser (precedence climbing).
    ParsedExprPtr ParseExpression();
    ParsedExprPtr ParseOr();
    ParsedExprPtr ParseAnd();
    ParsedExprPtr ParseNot();
    ParsedExprPtr ParseComparison();
    ParsedExprPtr ParseAddSub();
    ParsedExprPtr ParseMulDiv();
    ParsedExprPtr ParseUnary();
    ParsedExprPtr ParsePrimary();
    ParsedExprPtr ParseFunctionCall(const std::string &name);

    // FROM clause.
    std::unique_ptr<TableRef> ParseTableRef();

    // Type name (for CREATE TABLE and CAST).
    std::string ParseTypeName();

    // Token helpers.
    const Token &Current() const;
    const Token &Peek() const;
    const Token &Advance();
    bool Check(TokenType type) const;
    bool Match(TokenType type);
    const Token &Expect(TokenType type, const std::string &context);
    // Consume an IDENTIFIER or a non-reserved keyword (YEAR, MONTH, DAY, HOUR,
    // MINUTE, SECOND, EPOCH, DOW) — these tokenize as keywords for the sake of
    // DATE_TRUNC / EXTRACT but are legal as column names, aliases, etc.
    const Token &ExpectIdentifier(const std::string &context);
    static bool IsIdentifierOrNonReserved(TokenType t);
    bool IsAtEnd() const;

    // Keyword matching (case-insensitive by token type).
    bool CheckKeyword(TokenType kw) const;
    bool MatchKeyword(TokenType kw);

    [[noreturn]] void ThrowError(const std::string &msg);
    [[noreturn]] void ThrowUnexpected(const std::string &context);

    std::vector<Token> tokens_;
    size_t pos_;
    int expr_depth_ = 0;
    static constexpr int MAX_EXPR_DEPTH = 256;
};

} // namespace slothdb

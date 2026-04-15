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
    bool IsAtEnd() const;

    // Keyword matching (case-insensitive by token type).
    bool CheckKeyword(TokenType kw) const;
    bool MatchKeyword(TokenType kw);

    [[noreturn]] void ThrowError(const std::string &msg);
    [[noreturn]] void ThrowUnexpected(const std::string &context);

    std::vector<Token> tokens_;
    size_t pos_;
};

} // namespace slothdb

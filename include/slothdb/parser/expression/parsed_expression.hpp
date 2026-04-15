#pragma once

#include "slothdb/parser/tokenizer.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

class SelectStatement; // Forward declaration for subquery expressions.

enum class ExpressionType : uint8_t {
    COLUMN_REF,       // table.column or just column
    CONSTANT,         // 42, 'hello', 3.14, NULL, TRUE, FALSE
    FUNCTION,         // func(args...)
    COMPARISON,       // a > b
    CONJUNCTION,      // a AND b, a OR b
    NEGATION,         // NOT x
    IS_NULL,          // x IS NULL, x IS NOT NULL
    BETWEEN,          // x BETWEEN a AND b
    IN_LIST,          // x IN (1, 2, 3)
    CAST,             // CAST(x AS type)
    CASE,             // CASE WHEN ... THEN ... END
    STAR,             // * or table.*
    ARITHMETIC,       // a + b, a * b, etc.
    UNARY_MINUS,      // -x
    STRING_CONCAT,    // a || b
    WINDOW,           // func() OVER (PARTITION BY ... ORDER BY ...)
    SUBQUERY,         // EXISTS (SELECT ...), scalar subquery, IN (SELECT ...)
};

// Base AST node for parsed expressions.
class ParsedExpression {
public:
    explicit ParsedExpression(ExpressionType type) : type_(type) {}
    virtual ~ParsedExpression() = default;

    ExpressionType GetExpressionType() const { return type_; }

    // Optional alias (AS name).
    std::string alias;

private:
    ExpressionType type_;
};

using ParsedExprPtr = std::unique_ptr<ParsedExpression>;

// Column reference: column_name or table_name.column_name
class ColumnRefExpression : public ParsedExpression {
public:
    ColumnRefExpression(const std::string &column, const std::string &table = "")
        : ParsedExpression(ExpressionType::COLUMN_REF),
          column_name(column), table_name(table) {}

    std::string column_name;
    std::string table_name;
};

// Constant value: integer, float, string, null, bool.
class ConstantExpression : public ParsedExpression {
public:
    explicit ConstantExpression(const std::string &value, TokenType literal_type)
        : ParsedExpression(ExpressionType::CONSTANT),
          value(value), literal_type(literal_type) {}

    // For NULL.
    ConstantExpression()
        : ParsedExpression(ExpressionType::CONSTANT), is_null(true),
          literal_type(TokenType::INVALID) {}

    std::string value;
    TokenType literal_type = TokenType::INVALID;
    bool is_null = false;
};

// Function call: func_name(args...) with optional DISTINCT.
class FunctionExpression : public ParsedExpression {
public:
    FunctionExpression(const std::string &name, std::vector<ParsedExprPtr> args,
                       bool distinct = false)
        : ParsedExpression(ExpressionType::FUNCTION),
          function_name(name), arguments(std::move(args)), is_distinct(distinct) {}

    std::string function_name;
    std::vector<ParsedExprPtr> arguments;
    bool is_distinct;
};

// Comparison: left op right (=, <, >, <=, >=, !=, LIKE).
class ComparisonExpression : public ParsedExpression {
public:
    ComparisonExpression(const std::string &op, ParsedExprPtr left, ParsedExprPtr right)
        : ParsedExpression(ExpressionType::COMPARISON),
          op(op), left(std::move(left)), right(std::move(right)) {}

    std::string op;
    ParsedExprPtr left;
    ParsedExprPtr right;
};

// AND / OR.
class ConjunctionExpression : public ParsedExpression {
public:
    ConjunctionExpression(const std::string &op, ParsedExprPtr left, ParsedExprPtr right)
        : ParsedExpression(ExpressionType::CONJUNCTION),
          op(op), left(std::move(left)), right(std::move(right)) {}

    std::string op; // "AND" or "OR"
    ParsedExprPtr left;
    ParsedExprPtr right;
};

// NOT x.
class NegationExpression : public ParsedExpression {
public:
    explicit NegationExpression(ParsedExprPtr child)
        : ParsedExpression(ExpressionType::NEGATION), child(std::move(child)) {}

    ParsedExprPtr child;
};

// IS NULL / IS NOT NULL.
class IsNullExpression : public ParsedExpression {
public:
    IsNullExpression(ParsedExprPtr child, bool is_not)
        : ParsedExpression(ExpressionType::IS_NULL),
          child(std::move(child)), is_not(is_not) {}

    ParsedExprPtr child;
    bool is_not;
};

// Arithmetic: left op right (+, -, *, /, %).
class ArithmeticExpression : public ParsedExpression {
public:
    ArithmeticExpression(const std::string &op, ParsedExprPtr left, ParsedExprPtr right)
        : ParsedExpression(ExpressionType::ARITHMETIC),
          op(op), left(std::move(left)), right(std::move(right)) {}

    std::string op;
    ParsedExprPtr left;
    ParsedExprPtr right;
};

// Unary minus: -x.
class UnaryMinusExpression : public ParsedExpression {
public:
    explicit UnaryMinusExpression(ParsedExprPtr child)
        : ParsedExpression(ExpressionType::UNARY_MINUS), child(std::move(child)) {}

    ParsedExprPtr child;
};

// Star expression: * or table.*.
class StarExpression : public ParsedExpression {
public:
    StarExpression(const std::string &table = "")
        : ParsedExpression(ExpressionType::STAR), table_name(table) {}

    std::string table_name;
};

// CAST(expr AS type_name).
class CastExpression : public ParsedExpression {
public:
    CastExpression(ParsedExprPtr child, const std::string &type_name, bool is_try = false)
        : ParsedExpression(ExpressionType::CAST),
          child(std::move(child)), target_type(type_name), is_try(is_try) {}

    ParsedExprPtr child;
    std::string target_type;
    bool is_try = false;
};

// Subquery expression: EXISTS (SELECT ...), NOT EXISTS, scalar subquery, IN (SELECT ...).
enum class SubqueryType : uint8_t { EXISTS, NOT_EXISTS, SCALAR, IN_SUBQUERY };

class SubqueryExpression : public ParsedExpression {
public:
    SubqueryExpression(SubqueryType subquery_type,
                       std::unique_ptr<SelectStatement> subquery)
        : ParsedExpression(ExpressionType::SUBQUERY),
          subquery_type(subquery_type), subquery(std::move(subquery)) {}

    SubqueryType subquery_type;
    std::unique_ptr<SelectStatement> subquery;
    ParsedExprPtr child; // For IN subquery: the left-side expression.
};

// Forward declare SelectStatement for the header.
// (Already included via parsed_statement.hpp indirectly.)

// Window expression: func() OVER (PARTITION BY ... ORDER BY ...)
struct WindowOrderItem {
    ParsedExprPtr expression;
    bool ascending = true;
};

class WindowExpression : public ParsedExpression {
public:
    WindowExpression(const std::string &func_name, std::vector<ParsedExprPtr> args)
        : ParsedExpression(ExpressionType::WINDOW),
          function_name(func_name), arguments(std::move(args)) {}

    std::string function_name;
    std::vector<ParsedExprPtr> arguments;
    std::vector<ParsedExprPtr> partition_by;
    std::vector<WindowOrderItem> order_by;
};

} // namespace slothdb

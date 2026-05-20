#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/constants.hpp"
#include "slothdb/common/types/value.hpp"
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

enum class BoundExpressionType : uint8_t {
    COLUMN_REF,
    CONSTANT,
    COMPARISON,
    CONJUNCTION,
    NEGATION,
    IS_NULL,
    ARITHMETIC,
    UNARY_MINUS,
    FUNCTION,
    CAST,
    STAR,
    WINDOW,
    SUBQUERY
};

// Base class for bound (type-resolved) expressions.
class BoundExpression {
public:
    BoundExpression(BoundExpressionType type, LogicalType return_type)
        : type_(type), return_type_(std::move(return_type)) {}
    virtual ~BoundExpression() = default;

    BoundExpressionType GetExpressionType() const { return type_; }
    const LogicalType &GetReturnType() const { return return_type_; }
    // Used by the binder when promoting a literal to match the type of
    // the column it's compared with (avoids stod-per-row in the executor).
    void SetReturnType(LogicalType t) { return_type_ = std::move(t); }

    std::string alias;

private:
    BoundExpressionType type_;
    LogicalType return_type_;
};

using BoundExprPtr = std::unique_ptr<BoundExpression>;

// Bound column reference - resolved to table index + column index.
class BoundColumnRef : public BoundExpression {
public:
    BoundColumnRef(const std::string &column_name, idx_t column_index,
                   const LogicalType &type)
        : BoundExpression(BoundExpressionType::COLUMN_REF, type),
          column_name(column_name), column_index(column_index) {}

    std::string column_name;
    idx_t column_index;
};

// Bound constant value.
class BoundConstant : public BoundExpression {
public:
    explicit BoundConstant(Value val)
        : BoundExpression(BoundExpressionType::CONSTANT, val.type()),
          value(std::move(val)) {}

    Value value;
};

// Bound comparison (=, <, >, <=, >=, !=, LIKE).
class BoundComparison : public BoundExpression {
public:
    BoundComparison(const std::string &op, BoundExprPtr left, BoundExprPtr right)
        : BoundExpression(BoundExpressionType::COMPARISON, LogicalType::BOOLEAN()),
          op(op), left(std::move(left)), right(std::move(right)) {}

    std::string op;
    BoundExprPtr left;
    BoundExprPtr right;
};

// Bound AND/OR.
class BoundConjunction : public BoundExpression {
public:
    BoundConjunction(const std::string &op, BoundExprPtr left, BoundExprPtr right)
        : BoundExpression(BoundExpressionType::CONJUNCTION, LogicalType::BOOLEAN()),
          op(op), left(std::move(left)), right(std::move(right)) {}

    std::string op;
    BoundExprPtr left;
    BoundExprPtr right;
};

// Bound NOT.
class BoundNegation : public BoundExpression {
public:
    explicit BoundNegation(BoundExprPtr child)
        : BoundExpression(BoundExpressionType::NEGATION, LogicalType::BOOLEAN()),
          child(std::move(child)) {}

    BoundExprPtr child;
};

// Bound IS [NOT] NULL.
class BoundIsNull : public BoundExpression {
public:
    BoundIsNull(BoundExprPtr child, bool is_not)
        : BoundExpression(BoundExpressionType::IS_NULL, LogicalType::BOOLEAN()),
          child(std::move(child)), is_not(is_not) {}

    BoundExprPtr child;
    bool is_not;
};

// Bound arithmetic (+, -, *, /, %).
class BoundArithmetic : public BoundExpression {
public:
    BoundArithmetic(const std::string &op, BoundExprPtr left, BoundExprPtr right,
                    const LogicalType &result_type)
        : BoundExpression(BoundExpressionType::ARITHMETIC, result_type),
          op(op), left(std::move(left)), right(std::move(right)) {}

    std::string op;
    BoundExprPtr left;
    BoundExprPtr right;
};

// Bound unary minus.
class BoundUnaryMinus : public BoundExpression {
public:
    explicit BoundUnaryMinus(BoundExprPtr child)
        : BoundExpression(BoundExpressionType::UNARY_MINUS, child->GetReturnType()),
          child(std::move(child)) {}

    BoundExprPtr child;
};

// Bound function/aggregate call.
class BoundFunction : public BoundExpression {
public:
    BoundFunction(const std::string &name, std::vector<BoundExprPtr> args,
                  const LogicalType &return_type, bool is_aggregate, bool is_distinct = false,
                  BoundExprPtr filter_expr = nullptr)
        : BoundExpression(BoundExpressionType::FUNCTION, return_type),
          function_name(name), arguments(std::move(args)),
          is_aggregate(is_aggregate), is_distinct(is_distinct),
          filter(std::move(filter_expr)) {}

    std::string function_name;
    std::vector<BoundExprPtr> arguments;
    bool is_aggregate;
    bool is_distinct;
    // SQL:2003 FILTER (WHERE ...) on an aggregate. Per-row evaluated by
    // the aggregator and gating each update. NULL filter result acts as
    // false (matches SQL standard).
    BoundExprPtr filter;
};

// Bound CAST.
class BoundCast : public BoundExpression {
public:
    BoundCast(BoundExprPtr child, const LogicalType &target, bool is_try = false)
        : BoundExpression(BoundExpressionType::CAST, target),
          child(std::move(child)), is_try(is_try) {}

    BoundExprPtr child;
    bool is_try = false;
};

// Bound subquery expression (EXISTS, NOT EXISTS, IN subquery, scalar subquery).
// Stores the original SQL needed to re-execute the subquery.
class BoundSubqueryExpression : public BoundExpression {
public:
    enum class Type { EXISTS, NOT_EXISTS, SCALAR, IN_SUBQUERY };

    BoundSubqueryExpression(Type subtype, const LogicalType &return_type)
        : BoundExpression(BoundExpressionType::SUBQUERY, return_type),
          subtype(subtype) {}

    Type subtype;
    // Store the parsed SelectStatement so we can bind+execute at runtime.
    // We use a shared_ptr because expressions may be moved/copied.
    std::shared_ptr<void> parsed_query; // Actually SelectStatement*
    BoundExprPtr child; // For IN subquery: the left-side expression.
};

// Bound window function expression.
struct BoundWindowOrder {
    BoundExprPtr expression;
    bool ascending = true;
};

class BoundWindowExpression : public BoundExpression {
public:
    BoundWindowExpression(const std::string &name, std::vector<BoundExprPtr> args,
                          const LogicalType &return_type)
        : BoundExpression(BoundExpressionType::WINDOW, return_type),
          function_name(name), arguments(std::move(args)) {}

    std::string function_name;
    std::vector<BoundExprPtr> arguments;
    std::vector<BoundExprPtr> partition_by;
    std::vector<BoundWindowOrder> order_by;
    // SQL:2003 FILTER on window aggregates. Same per-row semantics as
    // BoundFunction::filter — gated update, NULL acts as false.
    BoundExprPtr filter;
};

} // namespace slothdb

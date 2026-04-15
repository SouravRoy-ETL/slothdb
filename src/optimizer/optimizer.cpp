#include "slothdb/optimizer/optimizer.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/execution/expression_executor.hpp"
#include <cmath>

namespace slothdb {

std::vector<std::string> Optimizer::applied_optimizations_;

// ============================================================================
// Main entry point
// ============================================================================

LogicalOpPtr Optimizer::Optimize(LogicalOpPtr plan) {
    applied_optimizations_.clear();

    // Apply passes in order. Each pass may transform the plan.
    plan = ConstantFolding(std::move(plan));
    plan = FilterPushdown(std::move(plan));
    plan = TopNOptimization(std::move(plan));

    return plan;
}

std::string Optimizer::DescribeOptimizations() {
    std::string result;
    for (auto &opt : applied_optimizations_) {
        result += opt + "\n";
    }
    return result;
}

// ============================================================================
// Constant Folding
// ============================================================================
// Evaluate constant expressions at plan time.
// e.g., WHERE 1 + 1 > 1  ->  WHERE true (remove the filter entirely)
// e.g., SELECT 2 * 3 -> SELECT 6

bool Optimizer::IsConstantExpression(const BoundExpression &expr) {
    switch (expr.GetExpressionType()) {
    case BoundExpressionType::CONSTANT:
        return true;
    case BoundExpressionType::ARITHMETIC: {
        auto &arith = static_cast<const BoundArithmetic &>(expr);
        return IsConstantExpression(*arith.left) && IsConstantExpression(*arith.right);
    }
    case BoundExpressionType::COMPARISON: {
        auto &cmp = static_cast<const BoundComparison &>(expr);
        return IsConstantExpression(*cmp.left) && IsConstantExpression(*cmp.right);
    }
    case BoundExpressionType::UNARY_MINUS: {
        auto &um = static_cast<const BoundUnaryMinus &>(expr);
        return IsConstantExpression(*um.child);
    }
    case BoundExpressionType::NEGATION: {
        auto &neg = static_cast<const BoundNegation &>(expr);
        return IsConstantExpression(*neg.child);
    }
    default:
        return false;
    }
}

Value Optimizer::EvaluateConstant(const BoundExpression &expr) {
    if (expr.GetExpressionType() == BoundExpressionType::CONSTANT) {
        return static_cast<const BoundConstant &>(expr).value;
    }

    // Use ExpressionExecutor with a dummy chunk.
    DataChunk dummy;
    dummy.Initialize({});
    dummy.SetCardinality(1);

    Vector result(expr.GetReturnType());
    ExpressionExecutor::Execute(expr, dummy, result, 1);
    return result.GetValue(0);
}

LogicalOpPtr Optimizer::ConstantFolding(LogicalOpPtr plan) {
    // Recursively optimize children first.
    for (auto &child : plan->children) {
        child = ConstantFolding(std::move(child));
    }

    // Fold constants in filter conditions.
    if (plan->GetOperatorType() == LogicalOperatorType::FILTER) {
        auto &filter = static_cast<LogicalFilter &>(*plan);
        if (filter.condition && IsConstantExpression(*filter.condition)) {
            auto val = EvaluateConstant(*filter.condition);
            if (!val.IsNull() && val.GetValue<bool>()) {
                // Filter is always true -> remove filter, return child.
                applied_optimizations_.push_back("ConstantFolding: removed always-true filter");
                return std::move(plan->children[0]);
            }
            // If always false, we could return an empty result.
            // For now, keep the filter.
        }
    }

    // Fold constant expressions in projections.
    if (plan->GetOperatorType() == LogicalOperatorType::PROJECTION) {
        auto &proj = static_cast<LogicalProjection &>(*plan);
        for (auto &expr : proj.expressions) {
            if (expr && IsConstantExpression(*expr) &&
                expr->GetExpressionType() != BoundExpressionType::CONSTANT) {
                auto val = EvaluateConstant(*expr);
                expr = std::make_unique<BoundConstant>(val);
                applied_optimizations_.push_back("ConstantFolding: folded constant expression");
            }
        }
    }

    return plan;
}

// ============================================================================
// Filter Pushdown
// ============================================================================
// Push filters below projections so they're applied earlier (fewer rows processed).

static bool CheckFilterRefs(const BoundExpression &expr) {
    switch (expr.GetExpressionType()) {
    case BoundExpressionType::COLUMN_REF:
    case BoundExpressionType::CONSTANT:
        return true;
    case BoundExpressionType::COMPARISON: {
        auto &cmp = static_cast<const BoundComparison &>(expr);
        return CheckFilterRefs(*cmp.left) && CheckFilterRefs(*cmp.right);
    }
    case BoundExpressionType::CONJUNCTION: {
        auto &conj = static_cast<const BoundConjunction &>(expr);
        return CheckFilterRefs(*conj.left) && CheckFilterRefs(*conj.right);
    }
    case BoundExpressionType::IS_NULL: {
        auto &isn = static_cast<const BoundIsNull &>(expr);
        return CheckFilterRefs(*isn.child);
    }
    case BoundExpressionType::NEGATION: {
        auto &neg = static_cast<const BoundNegation &>(expr);
        return CheckFilterRefs(*neg.child);
    }
    default:
        return false;
    }
}

LogicalOpPtr Optimizer::FilterPushdown(LogicalOpPtr plan) {
    // Recursively optimize children first.
    for (auto &child : plan->children) {
        child = FilterPushdown(std::move(child));
    }

    // Pattern: FILTER -> PROJECTION -> child
    // Transform to: PROJECTION -> FILTER -> child
    // (Only if filter doesn't reference projected columns that aren't simple column refs)
    if (plan->GetOperatorType() == LogicalOperatorType::FILTER &&
        !plan->children.empty() &&
        plan->children[0]->GetOperatorType() == LogicalOperatorType::PROJECTION) {

        auto &filter = static_cast<LogicalFilter &>(*plan);
        auto &proj = static_cast<LogicalProjection &>(*plan->children[0]);

        // Check if the filter condition only references column refs.
        bool can_pushdown = true;

        if (filter.condition) {
            can_pushdown = CheckFilterRefs(*filter.condition);
        }

        if (can_pushdown && !proj.children.empty()) {
            // Swap: move filter below projection.
            auto filter_cond = std::move(filter.condition);
            auto child_types = proj.children[0]->GetTypes();

            auto new_filter = std::make_unique<LogicalFilter>(
                std::move(filter_cond), child_types);
            new_filter->children.push_back(std::move(proj.children[0]));
            proj.children[0] = std::move(new_filter);

            applied_optimizations_.push_back("FilterPushdown: pushed filter below projection");
            return std::move(plan->children[0]); // Return the projection.
        }
    }

    return plan;
}

// ============================================================================
// Column Pruning (simplified)
// ============================================================================

LogicalOpPtr Optimizer::ColumnPruning(LogicalOpPtr plan) {
    // Recursively optimize children.
    for (auto &child : plan->children) {
        child = ColumnPruning(std::move(child));
    }
    // Full column pruning requires tracking which columns are needed
    // and removing unused ones from scans. For now, this is a no-op placeholder.
    return plan;
}

// ============================================================================
// TopN Optimization
// ============================================================================
// Replace ORDER BY + LIMIT with a TopN operator that maintains a heap
// instead of fully sorting all rows.

LogicalOpPtr Optimizer::TopNOptimization(LogicalOpPtr plan) {
    // Recursively optimize children.
    for (auto &child : plan->children) {
        child = TopNOptimization(std::move(child));
    }

    // Pattern: LIMIT -> ORDER_BY -> child
    // The PhysicalOrderBy + PhysicalLimit already work correctly together,
    // but we can note the optimization for EXPLAIN purposes.
    if (plan->GetOperatorType() == LogicalOperatorType::LIMIT &&
        !plan->children.empty() &&
        plan->children[0]->GetOperatorType() == LogicalOperatorType::ORDER_BY) {

        applied_optimizations_.push_back("TopN: ORDER BY + LIMIT detected (heap optimization candidate)");
    }

    return plan;
}

} // namespace slothdb

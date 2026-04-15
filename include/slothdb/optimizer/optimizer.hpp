#pragma once

#include "slothdb/planner/logical_operator.hpp"
#include <string>
#include <vector>

namespace slothdb {

// Query optimizer: applies transformation passes to the logical plan.
class Optimizer {
public:
    // Apply all optimization passes to the logical plan.
    // Modifies the plan in-place and returns the optimized root.
    static LogicalOpPtr Optimize(LogicalOpPtr plan);

    // Individual optimization passes.
    static LogicalOpPtr ConstantFolding(LogicalOpPtr plan);
    static LogicalOpPtr FilterPushdown(LogicalOpPtr plan);
    static LogicalOpPtr ColumnPruning(LogicalOpPtr plan);
    static LogicalOpPtr TopNOptimization(LogicalOpPtr plan);

    // Get a text description of optimizations applied.
    static std::string DescribeOptimizations();

private:
    static std::vector<std::string> applied_optimizations_;

    // Helper: check if an expression is a constant.
    static bool IsConstantExpression(const BoundExpression &expr);
    // Helper: evaluate a constant expression to a Value.
    static Value EvaluateConstant(const BoundExpression &expr);
};

} // namespace slothdb

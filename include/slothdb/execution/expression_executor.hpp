#pragma once

#include "slothdb/binder/bound_expression.hpp"
#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/vector.hpp"

namespace slothdb {

// Evaluates bound expressions against an input DataChunk,
// writing results into a target Vector.
class Catalog; // Forward declaration.

class ExpressionExecutor {
public:
    // Set the catalog for subquery execution.
    static void SetCatalog(Catalog *catalog) { catalog_ = catalog; }

    // Execute a single expression on the input chunk, writing to result.
    static void Execute(const BoundExpression &expr, DataChunk &input,
                        Vector &result, idx_t count);

    // Execute a full expression producing a Value (for constants).
    static Value ExecuteScalar(const BoundExpression &expr);

private:
    static void ExecuteColumnRef(const BoundColumnRef &expr, DataChunk &input,
                                 Vector &result, idx_t count);
    static void ExecuteConstant(const BoundConstant &expr, Vector &result, idx_t count);
    static void ExecuteComparison(const BoundComparison &expr, DataChunk &input,
                                  Vector &result, idx_t count);
    static void ExecuteConjunction(const BoundConjunction &expr, DataChunk &input,
                                   Vector &result, idx_t count);
    static void ExecuteArithmetic(const BoundArithmetic &expr, DataChunk &input,
                                  Vector &result, idx_t count);
    static void ExecuteNegation(const BoundNegation &expr, DataChunk &input,
                                Vector &result, idx_t count);
    static void ExecuteIsNull(const BoundIsNull &expr, DataChunk &input,
                              Vector &result, idx_t count);
    static void ExecuteIsBool(const BoundIsBool &expr, DataChunk &input,
                              Vector &result, idx_t count);
    static void ExecuteUnaryMinus(const BoundUnaryMinus &expr, DataChunk &input,
                                  Vector &result, idx_t count);
    static void ExecuteFunction(const BoundFunction &expr, DataChunk &input,
                                Vector &result, idx_t count);
    static void ExecuteCast(const BoundCast &expr, DataChunk &input,
                            Vector &result, idx_t count);
    static void ExecuteSubquery(const BoundSubqueryExpression &expr, DataChunk &input,
                                Vector &result, idx_t count);

    static Catalog *catalog_;

    // Helpers for typed operations.
    template <typename T>
    static void CompareTyped(const std::string &op, Vector &left, Vector &right,
                             Vector &result, idx_t count);

    template <typename T>
    static void ArithmeticTyped(const std::string &op, Vector &left, Vector &right,
                                Vector &result, idx_t count);
};

} // namespace slothdb

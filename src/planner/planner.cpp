#include "slothdb/planner/planner.hpp"
#include "slothdb/common/exception.hpp"

namespace slothdb {

namespace {

// Walks an expression tree and replaces any aggregate BoundFunction with a
// BoundColumnRef pointing at the aggregate operator's output position. Each
// hoisted aggregate is appended to `aggregates` (and its return type to
// `agg_types`); the column ref is at `group_count + agg_idx`. Used so that
// expressions like ROUND(AVG(x)), AVG(x)+1, and CAST(SUM(y) AS DOUBLE) get
// their inner aggregate evaluated by the aggregate operator instead of the
// scalar dispatcher (which doesn't know aggregates).
void HoistAggregates(BoundExprPtr &expr,
                     std::vector<BoundExprPtr> &aggregates,
                     std::vector<LogicalType> &agg_types,
                     idx_t group_count) {
    if (!expr) return;
    auto type = expr->GetExpressionType();

    if (type == BoundExpressionType::FUNCTION) {
        auto *fn = static_cast<BoundFunction *>(expr.get());
        if (fn->is_aggregate) {
            // Hoist this aggregate. Inner arguments are scalar (e.g.
            // AVG(x*2)); they're evaluated inside the aggregate operator,
            // not against the aggregate's output, so no rewrite needed.
            idx_t internal_pos = group_count + agg_types.size();
            auto ret = fn->GetReturnType();
            agg_types.push_back(ret);
            std::string name = fn->function_name;
            aggregates.push_back(std::move(expr));
            expr = std::make_unique<BoundColumnRef>(name, internal_pos, ret);
            return;
        }
        // Non-aggregate function: recurse into args.
        for (auto &arg : fn->arguments) HoistAggregates(arg, aggregates, agg_types, group_count);
        return;
    }
    if (type == BoundExpressionType::ARITHMETIC) {
        auto *a = static_cast<BoundArithmetic *>(expr.get());
        HoistAggregates(a->left, aggregates, agg_types, group_count);
        HoistAggregates(a->right, aggregates, agg_types, group_count);
        return;
    }
    if (type == BoundExpressionType::COMPARISON) {
        auto *a = static_cast<BoundComparison *>(expr.get());
        HoistAggregates(a->left, aggregates, agg_types, group_count);
        HoistAggregates(a->right, aggregates, agg_types, group_count);
        return;
    }
    if (type == BoundExpressionType::CONJUNCTION) {
        auto *a = static_cast<BoundConjunction *>(expr.get());
        HoistAggregates(a->left, aggregates, agg_types, group_count);
        HoistAggregates(a->right, aggregates, agg_types, group_count);
        return;
    }
    if (type == BoundExpressionType::NEGATION) {
        auto *a = static_cast<BoundNegation *>(expr.get());
        HoistAggregates(a->child, aggregates, agg_types, group_count);
        return;
    }
    if (type == BoundExpressionType::IS_NULL) {
        auto *a = static_cast<BoundIsNull *>(expr.get());
        HoistAggregates(a->child, aggregates, agg_types, group_count);
        return;
    }
    if (type == BoundExpressionType::UNARY_MINUS) {
        auto *a = static_cast<BoundUnaryMinus *>(expr.get());
        HoistAggregates(a->child, aggregates, agg_types, group_count);
        return;
    }
    if (type == BoundExpressionType::CAST) {
        auto *a = static_cast<BoundCast *>(expr.get());
        HoistAggregates(a->child, aggregates, agg_types, group_count);
        return;
    }
    // COLUMN_REF, CONSTANT, STAR, WINDOW, SUBQUERY: leave alone.
}

// Walks an expression tree and rewrites any source-schema BoundColumnRef that
// matches a GROUP BY column to the aggregate's group-output position. Stops
// at aggregate-internal column refs (those produced by HoistAggregates already
// address agg-internal schema and must not be remapped).
void RemapGroupColumns(BoundExprPtr &expr,
                       const std::vector<BoundExprPtr> &groups) {
    if (!expr) return;
    auto type = expr->GetExpressionType();

    if (type == BoundExpressionType::COLUMN_REF) {
        auto &cref = static_cast<BoundColumnRef &>(*expr);
        // Skip refs already in agg-internal schema. We mark those by giving
        // them column_index >= groups.size() during hoist, but agg outputs
        // could collide with source col_idx. Cheaper signal: hoisted refs
        // carry the function name; group remap targets source col names.
        // To keep it simple: only remap if we find a matching source idx
        // among the groups, and the ref is within source-schema range.
        for (idx_t gi = 0; gi < groups.size(); gi++) {
            if (!groups[gi]) continue;
            if (groups[gi]->GetExpressionType() != BoundExpressionType::COLUMN_REF) continue;
            auto &g_col = static_cast<BoundColumnRef &>(*groups[gi]);
            if (g_col.column_index == cref.column_index &&
                g_col.column_name == cref.column_name) {
                cref.column_index = gi;
                return;
            }
        }
        return;
    }
    if (type == BoundExpressionType::FUNCTION) {
        auto *fn = static_cast<BoundFunction *>(expr.get());
        // Aggregate args were evaluated inside the aggregate operator
        // against the SOURCE schema; leave their column refs alone.
        if (fn->is_aggregate) return;
        for (auto &arg : fn->arguments) RemapGroupColumns(arg, groups);
        return;
    }
    if (type == BoundExpressionType::ARITHMETIC) {
        auto *a = static_cast<BoundArithmetic *>(expr.get());
        RemapGroupColumns(a->left, groups);
        RemapGroupColumns(a->right, groups);
        return;
    }
    if (type == BoundExpressionType::COMPARISON) {
        auto *a = static_cast<BoundComparison *>(expr.get());
        RemapGroupColumns(a->left, groups);
        RemapGroupColumns(a->right, groups);
        return;
    }
    if (type == BoundExpressionType::CONJUNCTION) {
        auto *a = static_cast<BoundConjunction *>(expr.get());
        RemapGroupColumns(a->left, groups);
        RemapGroupColumns(a->right, groups);
        return;
    }
    if (type == BoundExpressionType::NEGATION) {
        auto *a = static_cast<BoundNegation *>(expr.get());
        RemapGroupColumns(a->child, groups);
        return;
    }
    if (type == BoundExpressionType::IS_NULL) {
        auto *a = static_cast<BoundIsNull *>(expr.get());
        RemapGroupColumns(a->child, groups);
        return;
    }
    if (type == BoundExpressionType::UNARY_MINUS) {
        auto *a = static_cast<BoundUnaryMinus *>(expr.get());
        RemapGroupColumns(a->child, groups);
        return;
    }
    if (type == BoundExpressionType::CAST) {
        auto *a = static_cast<BoundCast *>(expr.get());
        RemapGroupColumns(a->child, groups);
        return;
    }
}

} // namespace

LogicalOpPtr Planner::Plan(const BoundStatement &stmt) {
    switch (stmt.GetType()) {
    case BoundStatementType::SELECT:
        return PlanSelect(static_cast<const BoundSelectStatement &>(stmt));
    case BoundStatementType::INSERT:
        return PlanInsert(static_cast<const BoundInsertStatement &>(stmt));
    case BoundStatementType::CREATE_TABLE:
        return PlanCreateTable(static_cast<const BoundCreateTableStatement &>(stmt));
    case BoundStatementType::DROP_TABLE:
        return PlanDropTable(static_cast<const BoundDropTableStatement &>(stmt));
    case BoundStatementType::UPDATE:
        return PlanUpdate(static_cast<const BoundUpdateStatement &>(stmt));
    case BoundStatementType::DELETE_STMT:
        return PlanDelete(static_cast<const BoundDeleteStatement &>(stmt));
    default:
        throw InternalException("Unknown statement type in planner");
    }
}

LogicalOpPtr Planner::PlanSelect(const BoundSelectStatement &stmt) {
    // Build bottom-up: scan -> filter -> aggregate/project -> order -> limit.
    LogicalOpPtr plan;
    auto &mutable_stmt = const_cast<BoundSelectStatement &>(stmt);

    // 1. Source: table scan, join, or dummy scan.
    if (stmt.join && stmt.join->right_table) {
        // JOIN: create two scans and a LogicalJoin.
        auto left_scan = std::make_unique<LogicalGet>(stmt.table);
        auto right_scan = std::make_unique<LogicalGet>(stmt.join->right_table);

        // Determine join type.
        JoinType jt = JoinType::INNER;
        if (stmt.join->join_type == "LEFT") jt = JoinType::LEFT;
        else if (stmt.join->join_type == "RIGHT") jt = JoinType::RIGHT;
        else if (stmt.join->join_type == "FULL") jt = JoinType::FULL;
        else if (stmt.join->join_type == "CROSS") jt = JoinType::CROSS;

        // Combined types: left columns + right columns.
        auto combined_types = left_scan->GetTypes();
        auto right_types = right_scan->GetTypes();
        combined_types.insert(combined_types.end(), right_types.begin(), right_types.end());

        auto join_node = std::make_unique<LogicalJoin>(
            jt, std::move(mutable_stmt.join->condition), combined_types);
        join_node->children.push_back(std::move(left_scan));
        join_node->children.push_back(std::move(right_scan));
        plan = std::move(join_node);
    } else if (stmt.table) {
        plan = std::make_unique<LogicalGet>(stmt.table);
    } else {
        plan = std::make_unique<LogicalDummyScan>();
    }

    // 2. Filter (WHERE).
    if (stmt.where_clause) {
        auto filter = std::make_unique<LogicalFilter>(
            std::move(mutable_stmt.where_clause), plan->GetTypes());
        filter->children.push_back(std::move(plan));
        plan = std::move(filter);
    }

    // 3. Aggregation or Projection.
    //
    // Both the aggregation path and the plain-projection path defer the
    // projection until AFTER ORDER BY. That way:
    //  - For plain queries, ORDER BY reads source schema (otherwise a
    //    narrower projection would crash on a source col_idx).
    //  - For aggregate queries, ORDER BY reads aggregate-internal schema
    //    [groups..., aggregates...] so `ORDER BY ROUND(AVG(salary))`
    //    works after we hoist the aggregate. ORDER BY by alias re-binds
    //    the original select-list expression in the binder, so it gets
    //    the same hoist+remap treatment and addresses agg-internal
    //    schema correctly.
    std::unique_ptr<LogicalProjection> deferred_projection;
    if (stmt.has_aggregation) {
        std::vector<BoundExprPtr> groups;
        for (auto &g : mutable_stmt.group_by) {
            groups.push_back(std::move(g));
        }

        std::vector<BoundExprPtr> aggregates;
        std::vector<LogicalType> group_types;
        std::vector<LogicalType> agg_types;
        for (auto &g : groups) {
            group_types.push_back(g->GetReturnType());
        }

        std::vector<BoundExprPtr> proj_exprs;
        proj_exprs.reserve(mutable_stmt.select_list.size());

        for (auto &expr : mutable_stmt.select_list) {
            // Two-pass rewrite: hoist aggregates anywhere in the tree
            // (handles ROUND(AVG(x)), AVG(x)+1, CAST(SUM(y) AS DOUBLE)...),
            // then remap remaining source column refs that match a GROUP BY.
            HoistAggregates(expr, aggregates, agg_types, group_types.size());
            RemapGroupColumns(expr, groups);
            proj_exprs.push_back(std::move(expr));
        }

        // Same treatment for ORDER BY expressions so they read the same
        // aggregate-internal schema as the projection does.
        for (auto &item : mutable_stmt.order_by) {
            HoistAggregates(item.expression, aggregates, agg_types, group_types.size());
            RemapGroupColumns(item.expression, groups);
        }

        std::vector<LogicalType> agg_internal_types = group_types;
        agg_internal_types.insert(agg_internal_types.end(),
                                   agg_types.begin(), agg_types.end());

        auto agg = std::make_unique<LogicalAggregate>(
            std::move(groups), std::move(aggregates), std::move(agg_internal_types));
        agg->children.push_back(std::move(plan));
        plan = std::move(agg);

        deferred_projection = std::make_unique<LogicalProjection>(
            std::move(proj_exprs), stmt.result_types);
    } else if (stmt.has_window) {
        // Window function evaluation.
        auto window = std::make_unique<LogicalWindow>(
            std::move(mutable_stmt.select_list),
            std::move(mutable_stmt.qualify_clause),
            stmt.result_types);
        window->children.push_back(std::move(plan));
        plan = std::move(window);
    } else {
        deferred_projection = std::make_unique<LogicalProjection>(
            std::move(mutable_stmt.select_list), stmt.result_types);
    }

    // 4. ORDER BY - placed before the deferred projection so col_idx
    // refers to either source schema (plain queries) or aggregate-internal
    // schema (aggregate queries with hoisted aggregates).
    if (!stmt.order_by.empty()) {
        auto order = std::make_unique<LogicalOrderBy>(
            std::move(mutable_stmt.order_by), plan->GetTypes());
        order->children.push_back(std::move(plan));
        plan = std::move(order);
    }

    // Apply the deferred projection now. PhysicalDistinct preserves
    // first-occurrence order, so placing DISTINCT after projection still
    // yields the ORDER BY sort in the final output.
    if (deferred_projection) {
        deferred_projection->children.push_back(std::move(plan));
        plan = std::move(deferred_projection);
    }

    // 3b. DISTINCT - operates on projected output.
    if (stmt.is_distinct) {
        auto distinct = std::make_unique<LogicalDistinct>(plan->GetTypes());
        distinct->children.push_back(std::move(plan));
        plan = std::move(distinct);
    }

    // 5. LIMIT.
    if (stmt.limit_count >= 0) {
        auto limit = std::make_unique<LogicalLimit>(
            stmt.limit_count, stmt.offset_count, plan->GetTypes());
        limit->children.push_back(std::move(plan));
        plan = std::move(limit);
    }

    return plan;
}

LogicalOpPtr Planner::PlanInsert(const BoundInsertStatement &stmt) {
    auto &mutable_stmt = const_cast<BoundInsertStatement &>(stmt);
    return std::make_unique<LogicalInsert>(stmt.table, std::move(mutable_stmt.values));
}

LogicalOpPtr Planner::PlanCreateTable(const BoundCreateTableStatement &stmt) {
    return std::make_unique<LogicalCreateTable>(
        stmt.table_name, stmt.columns, stmt.if_not_exists);
}

LogicalOpPtr Planner::PlanDropTable(const BoundDropTableStatement &stmt) {
    return std::make_unique<LogicalDropTable>(stmt.table_name, stmt.if_exists);
}

LogicalOpPtr Planner::PlanUpdate(const BoundUpdateStatement &stmt) {
    auto &ms = const_cast<BoundUpdateStatement &>(stmt);
    return std::make_unique<LogicalUpdate>(
        stmt.table, std::move(ms.assignments), std::move(ms.where_clause));
}

LogicalOpPtr Planner::PlanDelete(const BoundDeleteStatement &stmt) {
    auto &ms = const_cast<BoundDeleteStatement &>(stmt);
    return std::make_unique<LogicalDeleteOp>(stmt.table, std::move(ms.where_clause));
}

} // namespace slothdb

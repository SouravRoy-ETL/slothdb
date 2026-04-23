#include "slothdb/planner/planner.hpp"
#include "slothdb/common/exception.hpp"

namespace slothdb {

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
    if (stmt.has_aggregation) {
        // The aggregate operator emits rows in [groups..., aggregates...]
        // order regardless of how the user wrote the SELECT list. To give
        // back the user's order (e.g. `SELECT COUNT(*), k ... GROUP BY k`
        // where the aggregate comes before the group column), we build a
        // Projection above the aggregate that picks columns out of the
        // aggregate's internal schema in SELECT-list order.
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
            bool handled = false;
            if (expr && expr->GetExpressionType() == BoundExpressionType::FUNCTION) {
                auto *fn = static_cast<BoundFunction *>(expr.get());
                if (fn->is_aggregate) {
                    idx_t internal_pos = group_types.size() + agg_types.size();
                    auto ret = fn->GetReturnType();
                    agg_types.push_back(ret);
                    proj_exprs.push_back(std::make_unique<BoundColumnRef>(
                        fn->function_name, internal_pos, ret));
                    aggregates.push_back(std::move(expr));
                    handled = true;
                }
            }
            if (!handled && expr &&
                expr->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                // Must be a reference to a GROUP BY column; locate its
                // position in the aggregate's internal schema.
                auto &cref = static_cast<BoundColumnRef &>(*expr);
                idx_t group_idx = INVALID_INDEX;
                for (idx_t gi = 0; gi < groups.size(); gi++) {
                    if (groups[gi] &&
                        groups[gi]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &g_col = static_cast<BoundColumnRef &>(*groups[gi]);
                        if (g_col.column_index == cref.column_index) {
                            group_idx = gi;
                            break;
                        }
                    }
                }
                if (group_idx != INVALID_INDEX) {
                    proj_exprs.push_back(std::make_unique<BoundColumnRef>(
                        cref.column_name, group_idx, cref.GetReturnType()));
                    handled = true;
                }
            }
            if (!handled) {
                // Fallback for shapes we don't yet classify (constants or
                // composite expressions in the select list). Forward the
                // expression unchanged — the projection evaluates it against
                // the aggregate's output, which won't be meaningful for
                // non-trivial cases but preserves prior behavior.
                proj_exprs.push_back(std::move(expr));
            }
        }

        std::vector<LogicalType> agg_internal_types = group_types;
        agg_internal_types.insert(agg_internal_types.end(),
                                   agg_types.begin(), agg_types.end());

        auto agg = std::make_unique<LogicalAggregate>(
            std::move(groups), std::move(aggregates), std::move(agg_internal_types));
        agg->children.push_back(std::move(plan));

        auto proj = std::make_unique<LogicalProjection>(
            std::move(proj_exprs), stmt.result_types);
        proj->children.push_back(std::move(agg));
        plan = std::move(proj);
    } else if (stmt.has_window) {
        // Window function evaluation.
        auto window = std::make_unique<LogicalWindow>(
            std::move(mutable_stmt.select_list),
            std::move(mutable_stmt.qualify_clause),
            stmt.result_types);
        window->children.push_back(std::move(plan));
        plan = std::move(window);
    }
    // For plain-projection queries we DEFER the projection until after
    // ORDER BY. The ORDER BY expressions bind against source-schema column
    // indices (the binder rewrites aliases back to their select-list
    // expression but keeps source col_idx), so sorting on the projected
    // output would read the wrong column — or crash when the projected
    // width is narrower than the bound col_idx.
    std::unique_ptr<LogicalProjection> deferred_projection;
    if (!stmt.has_aggregation && !stmt.has_window) {
        deferred_projection = std::make_unique<LogicalProjection>(
            std::move(mutable_stmt.select_list), stmt.result_types);
    }

    // 4. ORDER BY — placed before the deferred projection so col_idx
    // refers to source schema.
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

    // 3b. DISTINCT — operates on projected output.
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

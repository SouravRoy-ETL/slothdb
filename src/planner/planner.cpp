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
        // Separate the select list into group-by columns and aggregates.
        std::vector<BoundExprPtr> groups;
        std::vector<BoundExprPtr> aggregates;

        // Collect group-by expressions.
        for (auto &g : mutable_stmt.group_by) {
            groups.push_back(std::move(g));
        }

        // Collect aggregate expressions from select list.
        // Non-aggregate, non-group-by expressions in select list are
        // kept as-is (they should reference group-by columns).
        for (auto &expr : mutable_stmt.select_list) {
            if (expr && expr->GetExpressionType() == BoundExpressionType::FUNCTION) {
                auto *fn = static_cast<BoundFunction *>(expr.get());
                if (fn->is_aggregate) {
                    aggregates.push_back(std::move(expr));
                    continue;
                }
            }
            // Non-aggregate expression — if not already in groups, add as group.
            if (expr) {
                // Check if this is a group-by column already handled.
                bool is_group = false;
                for (auto &g : groups) {
                    if (g && expr->GetExpressionType() == BoundExpressionType::COLUMN_REF &&
                        g->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                        auto &e_col = static_cast<BoundColumnRef &>(*expr);
                        auto &g_col = static_cast<BoundColumnRef &>(*g);
                        if (e_col.column_index == g_col.column_index) {
                            is_group = true;
                            break;
                        }
                    }
                }
                if (!is_group && expr->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                    // This column should be in GROUP BY; treat as group.
                    // (In a stricter mode, we'd error here.)
                }
            }
        }

        // If no explicit GROUP BY but we have aggregates, it's a full-table agg.
        auto agg = std::make_unique<LogicalAggregate>(
            std::move(groups), std::move(aggregates), stmt.result_types);
        agg->children.push_back(std::move(plan));
        plan = std::move(agg);
    } else if (stmt.has_window) {
        // Window function evaluation.
        auto window = std::make_unique<LogicalWindow>(
            std::move(mutable_stmt.select_list),
            std::move(mutable_stmt.qualify_clause),
            stmt.result_types);
        window->children.push_back(std::move(plan));
        plan = std::move(window);
    } else {
        // Plain projection.
        auto proj = std::make_unique<LogicalProjection>(
            std::move(mutable_stmt.select_list), stmt.result_types);
        proj->children.push_back(std::move(plan));
        plan = std::move(proj);
    }

    // 3b. DISTINCT.
    if (stmt.is_distinct) {
        auto distinct = std::make_unique<LogicalDistinct>(plan->GetTypes());
        distinct->children.push_back(std::move(plan));
        plan = std::move(distinct);
    }

    // 4. ORDER BY.
    if (!stmt.order_by.empty()) {
        auto order = std::make_unique<LogicalOrderBy>(
            std::move(mutable_stmt.order_by), plan->GetTypes());
        order->children.push_back(std::move(plan));
        plan = std::move(order);
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

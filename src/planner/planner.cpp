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

// Structural equality for two bound-expression trees. Used to detect when
// a SELECT-list / ORDER-BY subtree matches a GROUP BY entry verbatim so we
// can replace the subtree with a column ref to the aggregate's group output
// (analogous to RemapGroupColumns for plain BoundColumnRef groups, but for
// expression-valued GROUP BY entries like DATE_PART('year', EventDate)).
// Conservative: only handles the node kinds present in the binder. Any kind
// not listed here returns false, which just disables the rewrite for that
// subtree (safe default - the planner falls back to evaluating the SELECT
// expression against the agg-output schema, which fails loudly).
bool BoundExprEqual(const BoundExpression *a, const BoundExpression *b) {
    if (!a || !b) return a == b;
    if (a->GetExpressionType() != b->GetExpressionType()) return false;
    switch (a->GetExpressionType()) {
    case BoundExpressionType::COLUMN_REF: {
        auto *ca = static_cast<const BoundColumnRef *>(a);
        auto *cb = static_cast<const BoundColumnRef *>(b);
        return ca->column_index == cb->column_index &&
               ca->column_name == cb->column_name;
    }
    case BoundExpressionType::CONSTANT: {
        auto *ca = static_cast<const BoundConstant *>(a);
        auto *cb = static_cast<const BoundConstant *>(b);
        return ca->value.ToString() == cb->value.ToString();
    }
    case BoundExpressionType::FUNCTION: {
        auto *fa = static_cast<const BoundFunction *>(a);
        auto *fb = static_cast<const BoundFunction *>(b);
        if (fa->function_name != fb->function_name) return false;
        if (fa->is_aggregate != fb->is_aggregate) return false;
        if (fa->is_distinct != fb->is_distinct) return false;
        if (fa->arguments.size() != fb->arguments.size()) return false;
        for (size_t i = 0; i < fa->arguments.size(); i++) {
            if (!BoundExprEqual(fa->arguments[i].get(), fb->arguments[i].get())) return false;
        }
        return true;
    }
    case BoundExpressionType::ARITHMETIC: {
        auto *aa = static_cast<const BoundArithmetic *>(a);
        auto *ab = static_cast<const BoundArithmetic *>(b);
        return aa->op == ab->op &&
               BoundExprEqual(aa->left.get(), ab->left.get()) &&
               BoundExprEqual(aa->right.get(), ab->right.get());
    }
    case BoundExpressionType::COMPARISON: {
        auto *aa = static_cast<const BoundComparison *>(a);
        auto *ab = static_cast<const BoundComparison *>(b);
        return aa->op == ab->op &&
               BoundExprEqual(aa->left.get(), ab->left.get()) &&
               BoundExprEqual(aa->right.get(), ab->right.get());
    }
    case BoundExpressionType::CONJUNCTION: {
        auto *aa = static_cast<const BoundConjunction *>(a);
        auto *ab = static_cast<const BoundConjunction *>(b);
        return aa->op == ab->op &&
               BoundExprEqual(aa->left.get(), ab->left.get()) &&
               BoundExprEqual(aa->right.get(), ab->right.get());
    }
    case BoundExpressionType::NEGATION: {
        auto *na = static_cast<const BoundNegation *>(a);
        auto *nb = static_cast<const BoundNegation *>(b);
        return BoundExprEqual(na->child.get(), nb->child.get());
    }
    case BoundExpressionType::IS_NULL: {
        auto *na = static_cast<const BoundIsNull *>(a);
        auto *nb = static_cast<const BoundIsNull *>(b);
        return na->is_not == nb->is_not &&
               BoundExprEqual(na->child.get(), nb->child.get());
    }
    case BoundExpressionType::UNARY_MINUS: {
        auto *na = static_cast<const BoundUnaryMinus *>(a);
        auto *nb = static_cast<const BoundUnaryMinus *>(b);
        return BoundExprEqual(na->child.get(), nb->child.get());
    }
    case BoundExpressionType::CAST: {
        auto *ca = static_cast<const BoundCast *>(a);
        auto *cb = static_cast<const BoundCast *>(b);
        if (!(ca->GetReturnType() == cb->GetReturnType())) return false;
        return ca->is_try == cb->is_try &&
               BoundExprEqual(ca->child.get(), cb->child.get());
    }
    default:
        return false;
    }
}

// Walks a SELECT-list / ORDER-BY expression top-down. If a subtree matches
// (structurally) one of the original GROUP BY expressions, replace it with
// a BoundColumnRef pointing at the aggregate operator's group-output slot
// for that group (column_index = group_index, name = "_gbe_<i>"). This
// turns `SELECT date_part('year', EventDate), COUNT(*) ... GROUP BY 1`
// into a plan whose projection reads the precomputed group value rather
// than re-evaluating date_part against the post-aggregate schema (which
// no longer contains EventDate).
//
// Run BEFORE HoistAggregates/RemapGroupColumns: a top-level GROUP BY match
// short-circuits the descent into aggregates, which is what we want.
void RewriteGroupExprs(BoundExprPtr &expr,
                       const std::vector<const BoundExpression *> &original_groups,
                       const std::vector<LogicalType> &group_types) {
    if (!expr) return;
    // Try whole-subtree match first.
    for (idx_t gi = 0; gi < original_groups.size(); gi++) {
        if (!original_groups[gi]) continue;
        // Skip plain COLUMN_REF groups - RemapGroupColumns already handles
        // those, and matching them here would double-rewrite the index.
        if (original_groups[gi]->GetExpressionType() == BoundExpressionType::COLUMN_REF) continue;
        if (BoundExprEqual(expr.get(), original_groups[gi])) {
            expr = std::make_unique<BoundColumnRef>(
                std::string("_gbe_") + std::to_string(gi), gi, group_types[gi]);
            return;
        }
    }
    // No whole-subtree match - recurse.
    auto type = expr->GetExpressionType();
    if (type == BoundExpressionType::FUNCTION) {
        auto *fn = static_cast<BoundFunction *>(expr.get());
        for (auto &arg : fn->arguments) RewriteGroupExprs(arg, original_groups, group_types);
        return;
    }
    if (type == BoundExpressionType::ARITHMETIC) {
        auto *a = static_cast<BoundArithmetic *>(expr.get());
        RewriteGroupExprs(a->left, original_groups, group_types);
        RewriteGroupExprs(a->right, original_groups, group_types);
        return;
    }
    if (type == BoundExpressionType::COMPARISON) {
        auto *a = static_cast<BoundComparison *>(expr.get());
        RewriteGroupExprs(a->left, original_groups, group_types);
        RewriteGroupExprs(a->right, original_groups, group_types);
        return;
    }
    if (type == BoundExpressionType::CONJUNCTION) {
        auto *a = static_cast<BoundConjunction *>(expr.get());
        RewriteGroupExprs(a->left, original_groups, group_types);
        RewriteGroupExprs(a->right, original_groups, group_types);
        return;
    }
    if (type == BoundExpressionType::NEGATION) {
        auto *a = static_cast<BoundNegation *>(expr.get());
        RewriteGroupExprs(a->child, original_groups, group_types);
        return;
    }
    if (type == BoundExpressionType::IS_NULL) {
        auto *a = static_cast<BoundIsNull *>(expr.get());
        RewriteGroupExprs(a->child, original_groups, group_types);
        return;
    }
    if (type == BoundExpressionType::UNARY_MINUS) {
        auto *a = static_cast<BoundUnaryMinus *>(expr.get());
        RewriteGroupExprs(a->child, original_groups, group_types);
        return;
    }
    if (type == BoundExpressionType::CAST) {
        auto *a = static_cast<BoundCast *>(expr.get());
        RewriteGroupExprs(a->child, original_groups, group_types);
        return;
    }
    // COLUMN_REF, CONSTANT, STAR, WINDOW, SUBQUERY: no rewrite needed.
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

    // 1. Source: table scan, join chain, or dummy scan.
    // For an N-table chain (SQL-92 comma-FROM or chained explicit JOINs),
    // fold the bound joins vector into a left-deep cascade of LogicalJoin
    // nodes. `plan` starts at the leftmost LogicalGet and grows leftward
    // (each iteration wraps the current plan as the left child of a new
    // LogicalJoin whose right child is the next table's LogicalGet).
    if (!stmt.joins.empty() && stmt.joins.front()->right_table) {
        plan = std::make_unique<LogicalGet>(stmt.table);
        auto combined_types = plan->GetTypes();
        for (auto &join_info_ptr : mutable_stmt.joins) {
            auto &join_info = *join_info_ptr;
            if (!join_info.right_table) break;
            auto right_scan = std::make_unique<LogicalGet>(join_info.right_table);
            auto right_types = right_scan->GetTypes();
            combined_types.insert(combined_types.end(),
                                  right_types.begin(), right_types.end());

            JoinType jt = JoinType::INNER;
            if (join_info.join_type == "LEFT") jt = JoinType::LEFT;
            else if (join_info.join_type == "RIGHT") jt = JoinType::RIGHT;
            else if (join_info.join_type == "FULL") jt = JoinType::FULL;
            else if (join_info.join_type == "CROSS") jt = JoinType::CROSS;

            auto join_node = std::make_unique<LogicalJoin>(
                jt, std::move(join_info.condition), combined_types);
            join_node->children.push_back(std::move(plan));
            join_node->children.push_back(std::move(right_scan));
            plan = std::move(join_node);
        }
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

        // GROUP BY-by-expression lift. For each non-COLUMN_REF GROUP BY
        // entry (e.g. DATE_PART('year', EventDate)), insert a passthrough
        // pre-aggregate LogicalProjection that materializes the expression
        // as a synthetic source column, then replace the GROUP BY entry
        // with a BoundColumnRef pointing at that synthetic column. The
        // aggregate operator then reads the precomputed value instead of
        // re-evaluating the expression against post-aggregate schema (which
        // is missing the original source columns).
        //
        // We also save raw pointers to the ORIGINAL group expressions
        // (kept alive inside the lift projection) so RewriteGroupExprs
        // below can match SELECT-list / ORDER-BY subtrees against them.
        std::vector<const BoundExpression *> original_group_ptrs;
        original_group_ptrs.reserve(groups.size());
        for (auto &g : groups) original_group_ptrs.push_back(g.get());

        // redundant-group elimination. GROUP BY (X, X±c1, X±c2, ...)
        // has the same partition as GROUP BY X — X uniquely determines X±c.
        // Drop the (col ± const) groups when col itself is also a group; the
        // SELECT-list/ORDER-BY rewrites that match against original_group_ptrs
        // still find the dropped expression, but RemapGroupColumns will rewrite
        // its inner col-ref to the surviving aggregate-output slot, so the
        // arithmetic is computed only on the (small) post-aggregate row count
        // instead of every input row. The result hashes 1 col, not 4.
        {
            std::vector<idx_t> kept_colref_idx; // source col_idx of COLUMN_REF groups
            kept_colref_idx.reserve(groups.size());
            for (auto &g : groups) {
                if (g->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                    kept_colref_idx.push_back(
                        static_cast<const BoundColumnRef &>(*g).column_index);
                }
            }
            auto base_col_of_arith = [](const BoundExpression &e, idx_t &out_col) -> bool {
                if (e.GetExpressionType() != BoundExpressionType::ARITHMETIC) return false;
                auto &a = static_cast<const BoundArithmetic &>(e);
                bool lc = a.left && a.left->GetExpressionType() == BoundExpressionType::COLUMN_REF;
                bool rc = a.right && a.right->GetExpressionType() == BoundExpressionType::COLUMN_REF;
                bool lk = a.left && a.left->GetExpressionType() == BoundExpressionType::CONSTANT;
                bool rk = a.right && a.right->GetExpressionType() == BoundExpressionType::CONSTANT;
                if (lc && rk) {
                    out_col = static_cast<const BoundColumnRef &>(*a.left).column_index;
                    return true;
                }
                if (lk && rc) {
                    out_col = static_cast<const BoundColumnRef &>(*a.right).column_index;
                    return true;
                }
                return false;
            };
            std::vector<bool> drop(groups.size(), false);
            for (idx_t gi = 0; gi < groups.size(); gi++) {
                idx_t base_col = 0;
                if (!base_col_of_arith(*groups[gi], base_col)) continue;
                for (idx_t kc : kept_colref_idx) {
                    if (kc == base_col) { drop[gi] = true; break; }
                }
            }
            // Constant group cols are redundant when ANY non-constant
            // group col is also present. `GROUP BY 1, URL` becomes
            // `GROUP BY URL` since the literal contributes no partition.
            // Without this drop the 2-col packed-path explodes to 10M+
            // (URL, 1) pairs and the FUSED GENERIC merge OOMs.
            bool has_non_constant = false;
            for (idx_t gi = 0; gi < groups.size(); gi++) {
                if (drop[gi]) continue;
                if (groups[gi]->GetExpressionType() !=
                    BoundExpressionType::CONSTANT) {
                    has_non_constant = true;
                    break;
                }
            }
            if (has_non_constant) {
                for (idx_t gi = 0; gi < groups.size(); gi++) {
                    if (drop[gi]) continue;
                    if (groups[gi]->GetExpressionType() ==
                        BoundExpressionType::CONSTANT) {
                        drop[gi] = true;
                    }
                }
            }
            // Erase back-to-front. original_group_ptrs[gi] points into
            // groups[gi]; erasing groups[gi] frees the BoundExpression, so
            // we must drop the matching original_group_ptrs entry too — but
            // RewriteGroupExprs/RemapGroupColumns won't ever try to match
            // against a dropped expression because we also remove its slot.
            for (idx_t gi = groups.size(); gi-- > 0;) {
                if (!drop[gi]) continue;
                groups.erase(groups.begin() + gi);
                group_types.erase(group_types.begin() + gi);
                original_group_ptrs.erase(original_group_ptrs.begin() + gi);
            }
        }

        bool needs_lift = false;
        for (auto &g : groups) {
            if (g->GetExpressionType() != BoundExpressionType::COLUMN_REF) {
                needs_lift = true;
                break;
            }
        }
        if (needs_lift) {
            const auto &source_types = plan->GetTypes();
            idx_t source_w = source_types.size();
            std::vector<BoundExprPtr> lift_exprs;
            std::vector<LogicalType> lift_types;
            lift_exprs.reserve(source_w + groups.size());
            lift_types.reserve(source_w + groups.size());
            // Passthrough: column_index i with the placeholder name (the
            // executor only looks at index, not name, for COLUMN_REF).
            for (idx_t i = 0; i < source_w; i++) {
                lift_exprs.push_back(std::make_unique<BoundColumnRef>(
                    std::string("_pt_") + std::to_string(i), i, source_types[i]));
                lift_types.push_back(source_types[i]);
            }
            // Lift each non-COLUMN_REF group expression. Replace groups[gi]
            // with a synthetic column ref pointing at the lifted slot.
            // original_group_ptrs[gi] still points to the original tree -
            // it's been moved into lift_exprs but the heap object is the
            // same so the pointer stays valid.
            for (idx_t gi = 0; gi < groups.size(); gi++) {
                if (groups[gi]->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
                    continue;
                }
                idx_t synthetic_idx = lift_exprs.size();
                LogicalType gt = group_types[gi];
                lift_exprs.push_back(std::move(groups[gi]));
                lift_types.push_back(gt);
                groups[gi] = std::make_unique<BoundColumnRef>(
                    std::string("_gbe_") + std::to_string(gi), synthetic_idx, gt);
            }
            auto lift_proj = std::make_unique<LogicalProjection>(
                std::move(lift_exprs), std::move(lift_types));
            lift_proj->children.push_back(std::move(plan));
            plan = std::move(lift_proj);
        }

        std::vector<BoundExprPtr> proj_exprs;
        proj_exprs.reserve(mutable_stmt.select_list.size());

        for (auto &expr : mutable_stmt.select_list) {
            // Three-pass rewrite for SELECT list under aggregation:
            //  1. RewriteGroupExprs: replace any subtree that matches a
            //     non-COLUMN_REF GROUP BY entry with a column ref to the
            //     aggregate's group-output slot (handles `SELECT
            //     date_part('year', EventDate) ... GROUP BY 1`).
            //  2. HoistAggregates: pull aggregates out anywhere in the
            //     tree (ROUND(AVG(x)), AVG(x)+1, CAST(SUM(y) AS DOUBLE)).
            //  3. RemapGroupColumns: remap remaining source column refs
            //     that match a plain COLUMN_REF GROUP BY entry.
            RewriteGroupExprs(expr, original_group_ptrs, group_types);
            HoistAggregates(expr, aggregates, agg_types, group_types.size());
            RemapGroupColumns(expr, groups);
            proj_exprs.push_back(std::move(expr));
        }

        // Same treatment for ORDER BY expressions so they read the same
        // aggregate-internal schema as the projection does.
        for (auto &item : mutable_stmt.order_by) {
            RewriteGroupExprs(item.expression, original_group_ptrs, group_types);
            HoistAggregates(item.expression, aggregates, agg_types, group_types.size());
            RemapGroupColumns(item.expression, groups);
        }

        // HAVING rewrite - same three passes as SELECT/ORDER BY so the
        // expression reads the aggregate's [groups..., aggregates...]
        // internal schema. Must run BEFORE LogicalAggregate construction
        // because HoistAggregates may append new aggregates that the
        // operator needs to evaluate. RemapGroupColumns reads `groups`
        // by reference, so we do it before moving groups into the agg.
        if (mutable_stmt.having_clause) {
            RewriteGroupExprs(mutable_stmt.having_clause,
                              original_group_ptrs, group_types);
            HoistAggregates(mutable_stmt.having_clause, aggregates,
                            agg_types, group_types.size());
            RemapGroupColumns(mutable_stmt.having_clause, groups);
        }

        std::vector<LogicalType> agg_internal_types = group_types;
        agg_internal_types.insert(agg_internal_types.end(),
                                   agg_types.begin(), agg_types.end());

        auto agg_types_for_filter = agg_internal_types;
        auto agg = std::make_unique<LogicalAggregate>(
            std::move(groups), std::move(aggregates), std::move(agg_internal_types));
        agg->children.push_back(std::move(plan));
        plan = std::move(agg);

        // Insert the HAVING filter directly above the aggregate. ORDER BY
        // and the deferred projection still read the same agg-internal
        // schema, so they keep working.
        if (mutable_stmt.having_clause) {
            auto having_filter = std::make_unique<LogicalFilter>(
                std::move(mutable_stmt.having_clause), agg_types_for_filter);
            having_filter->children.push_back(std::move(plan));
            plan = std::move(having_filter);
        }

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

    // 5. LIMIT / OFFSET. Emit a LogicalLimit when either is present.
    // The pre-fix guard only ran on `limit_count >= 0`, so a bare
    // `OFFSET 2` with no LIMIT silently dropped the OFFSET and
    // returned every row. PhysicalLimit already handles limit=-1 with
    // a positive offset correctly (no upper bound, skips offset rows).
    if (stmt.limit_count >= 0 || stmt.offset_count > 0) {
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

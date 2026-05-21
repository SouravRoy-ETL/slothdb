#include "slothdb/binder/binder.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include "slothdb/parser/expression/parsed_expression.hpp"

namespace slothdb {

static bool ContainsAggregate(const BoundExpression &expr);

// ============================================================================
// BindContext
// ============================================================================

void BindContext::AddTable(const std::string &alias, TableCatalogEntry *entry) {
    tables[alias] = entry;
    // Compute offset: sum of all columns from previously added tables.
    idx_t offset = 0;
    for (auto &[tbl_alias, tbl_offset] : table_order) {
        offset = tbl_offset + tables[tbl_alias]->ColumnCount();
    }
    table_order.push_back({alias, offset});

    for (idx_t i = 0; i < entry->ColumnCount(); i++) {
        auto &col = entry->GetColumns()[i];
        idx_t combined_idx = offset + i;
        if (columns.count(col.name) && columns[col.name].first != alias) {
            // Mark as ambiguous.
            columns[col.name] = {"", INVALID_INDEX};
        } else {
            columns[col.name] = {alias, combined_idx};
        }
    }
}

std::pair<std::string, idx_t> BindContext::ResolveColumn(
    const std::string &col_name, const std::string &table_name) const {

    if (!table_name.empty()) {
        auto it = tables.find(table_name);
        if (it == tables.end()) {
            throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                                   "Table '" + table_name + "' not found");
        }
        auto idx = it->second->GetColumnIndex(col_name);
        if (idx == INVALID_INDEX) {
            throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                                   "Column '" + col_name + "' not found in table '" + table_name + "'");
        }
        // Add table offset for JOIN context.
        idx += GetTableOffset(table_name);
        return {table_name, idx};
    }

    auto it = columns.find(col_name);
    if (it == columns.end()) {
        throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                               "Column '" + col_name + "' not found");
    }
    if (it->second.second == INVALID_INDEX) {
        throw BinderException(ErrorCode::AMBIGUOUS_COLUMN,
                               "Column '" + col_name + "' is ambiguous");
    }
    return it->second;
}

LogicalType BindContext::GetColumnType(const std::string &col_name,
                                        const std::string &table_name) const {
    auto [tbl, combined_idx] = ResolveColumn(col_name, table_name);
    // Get the local index within the table.
    idx_t offset = GetTableOffset(tbl);
    idx_t local_idx = combined_idx - offset;
    return tables.at(tbl)->GetColumns()[local_idx].type;
}

idx_t BindContext::GetTableOffset(const std::string &alias) const {
    for (auto &[tbl_alias, offset] : table_order) {
        if (tbl_alias == alias) return offset;
    }
    return 0;
}

// ============================================================================
// Binder
// ============================================================================

Binder::Binder(Catalog &catalog) : catalog_(catalog) {}

BoundStmtPtr Binder::Bind(const ParsedStatement &stmt) {
    switch (stmt.GetType()) {
    case StatementType::SELECT:
        return BindSelect(static_cast<const SelectStatement &>(stmt));
    case StatementType::CREATE_TABLE:
        return BindCreateTable(static_cast<const CreateTableStatement &>(stmt));
    case StatementType::DROP_TABLE:
        return BindDropTable(static_cast<const DropTableStatement &>(stmt));
    case StatementType::INSERT:
        return BindInsert(static_cast<const InsertStatement &>(stmt));
    case StatementType::UPDATE:
        return BindUpdate(static_cast<const UpdateStatement &>(stmt));
    case StatementType::DELETE_STMT:
        return BindDelete(static_cast<const DeleteStatement &>(stmt));
    default:
        throw InternalException("Unknown statement type in binder");
    }
}

// ============================================================================
// Bind SELECT
// ============================================================================

BoundStmtPtr Binder::BindSelect(const SelectStatement &stmt) {
    auto result = std::make_unique<BoundSelectStatement>();
    result->is_distinct = stmt.is_distinct;
    BindContext context;

    // Bind FROM.
    if (stmt.from_table) {
        BindTableRef(*stmt.from_table, context);
        // Set primary table.
        if (!context.tables.empty()) {
            // Use the left-most table name.
            std::string primary_name = stmt.from_table->table_name;
            std::string primary_alias = stmt.from_table->alias.empty()
                ? primary_name : stmt.from_table->alias;
            auto it = context.tables.find(primary_alias);
            if (it != context.tables.end()) {
                result->table = it->second;
                result->table_alias = it->first;
            } else {
                auto first = context.tables.begin();
                result->table = first->second;
                result->table_alias = first->first;
            }

            // Detect JOIN chain. SQL-92 comma-FROM (`FROM a, b, c`) plus
            // explicit JOINs both walk the right-leaning chain on the
            // parsed TableRef. Each chain link contributes one JoinInfo;
            // the planner folds them into a left-deep cascade. The
            // running column-count tracks the cumulative width of the
            // left side so each JoinInfo's left_col_count is meaningful
            // for downstream binder/planner code that uses it.
            idx_t running_left_count = result->table->ColumnCount();
            const TableRef *cur = stmt.from_table.get();
            while (cur->right && !cur->join_type.empty()) {
                auto join_info = std::make_unique<BoundSelectStatement::JoinInfo>();
                auto right_alias = cur->right->alias.empty()
                    ? cur->right->table_name : cur->right->alias;
                auto right_it = context.tables.find(right_alias);
                if (right_it != context.tables.end()) {
                    join_info->right_table = right_it->second;
                    join_info->right_alias = right_alias;
                }
                join_info->join_type = cur->join_type;
                join_info->left_col_count = running_left_count;
                join_info->right_col_count = join_info->right_table
                    ? join_info->right_table->ColumnCount() : 0;

                if (cur->on_condition) {
                    join_info->condition = BindExpression(*cur->on_condition, context);
                }

                running_left_count += join_info->right_col_count;
                result->joins.push_back(std::move(join_info));
                cur = cur->right.get();
            }
        }
    }

    // Bind select list - expand stars.
    for (auto &expr : stmt.select_list) {
        if (expr->GetExpressionType() == ExpressionType::STAR) {
            auto &star = static_cast<const StarExpression &>(*expr);
            std::vector<BoundExprPtr> expanded;
            BindStar(star, context, expanded);
            for (auto &e : expanded) {
                result->result_names.push_back(e->alias);
                result->result_types.push_back(e->GetReturnType());
                result->select_list.push_back(std::move(e));
            }
        } else {
            auto bound = BindExpression(*expr, context);
            std::string name = expr->alias.empty()
                ? (expr->GetExpressionType() == ExpressionType::COLUMN_REF
                    ? static_cast<const ColumnRefExpression &>(*expr).column_name
                    : "expr")
                : expr->alias;
            bound->alias = name;
            result->result_names.push_back(name);
            result->result_types.push_back(bound->GetReturnType());

            // Track aggregates and window functions.
            if (bound->GetExpressionType() == BoundExpressionType::FUNCTION) {
                auto *fn = static_cast<BoundFunction *>(bound.get());
                if (fn->is_aggregate) {
                    result->aggregates.push_back(fn);
                    result->has_aggregation = true;
                }
            }
            if (bound->GetExpressionType() == BoundExpressionType::WINDOW) {
                result->has_window = true;
            }

            result->select_list.push_back(std::move(bound));
        }
    }

    // Bind WHERE. Reject aggregates in WHERE per SQL standard — they
    // were silently routed to the executor before, surfacing as
    // "NotImplemented Error: Function execution for: COUNT" which
    // misleadingly suggested COUNT itself was missing. Clean
    // BinderException is what SQL standard requires.
    if (stmt.where_clause) {
        result->where_clause = BindExpression(*stmt.where_clause, context);
        if (ContainsAggregate(*result->where_clause)) {
            throw BinderException(ErrorCode::AGGREGATE_IN_WHERE,
                "aggregate functions are not allowed in WHERE clause");
        }
    }

    // Bind GROUP BY. Resolve positional ordinals (`GROUP BY 1`) and
    // select-list aliases (e.g. `extract(minute FROM EventTime) AS m ...
    // GROUP BY m`) to their underlying SELECT-list expression. Some
    // queries need the ordinal path; others need the alias path.
    for (auto &expr : stmt.group_by) {
        bool resolved = false;
        // Positional ordinal: GROUP BY <integer-literal>
        if (expr->GetExpressionType() == ExpressionType::CONSTANT) {
            auto bound = BindExpression(*expr, context);
            if (bound->GetExpressionType() == BoundExpressionType::CONSTANT) {
                auto &val = static_cast<BoundConstant &>(*bound).value;
                int64_t ord = -1;
                if (val.type().id() == LogicalTypeId::INTEGER) {
                    ord = val.GetValue<int32_t>();
                } else if (val.type().id() == LogicalTypeId::BIGINT) {
                    ord = val.GetValue<int64_t>();
                }
                // Reference the post-expansion select_list so that
                // `SELECT * FROM t GROUP BY 1, 2` resolves to actual
                // expanded columns rather than the STAR placeholder.
                // Previously this used stmt.select_list.size() (pre-
                // expansion = 1 for `*`) and silently fell through
                // when ord matched a STAR, leaving trailing GROUP BY
                // columns dangling with corrupt aggregate output.
                if (ord >= 1 && ord <= static_cast<int64_t>(result->select_list.size())) {
                    idx_t sl_idx = static_cast<idx_t>(ord - 1);
                    // result->select_list[sl_idx] is already a bound
                    // expression (column ref / arithmetic / etc) —
                    // reuse it directly by re-binding the original
                    // parsed expression when available, or wrap a
                    // bound copy via the result_names index for
                    // star-expanded columns. The simplest correct
                    // path: peek at the source. If the ordinal points
                    // beyond stmt.select_list (i.e., into a star-
                    // expanded slot), look up the column by name from
                    // the bind context using result_names[sl_idx];
                    // otherwise re-bind from stmt.select_list when
                    // it's a non-STAR scalar.
                    if (sl_idx < stmt.select_list.size() &&
                        stmt.select_list[sl_idx]->GetExpressionType()
                            != ExpressionType::STAR) {
                        result->group_by.push_back(
                            BindExpression(*stmt.select_list[sl_idx], context));
                        resolved = true;
                    } else if (sl_idx < result->result_names.size()) {
                        // Star-expanded column: re-resolve by name.
                        auto col = std::make_unique<ColumnRefExpression>(
                            result->result_names[sl_idx]);
                        try {
                            result->group_by.push_back(BindExpression(*col, context));
                            resolved = true;
                        } catch (...) {
                            // Fall through to alias-name path.
                        }
                    }
                }
            }
        }
        // Alias: GROUP BY <result-name>
        if (!resolved && expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
            auto &col_ref = static_cast<ColumnRefExpression &>(*expr);
            if (col_ref.table_name.empty()) {
                auto ref_upper = StringUtil::Upper(col_ref.column_name);
                for (idx_t i = 0; i < result->result_names.size(); i++) {
                    if (StringUtil::Upper(result->result_names[i]) == ref_upper) {
                        idx_t sl_idx = std::min<idx_t>(i, stmt.select_list.size() - 1);
                        if (stmt.select_list[sl_idx]->GetExpressionType()
                                == ExpressionType::STAR) {
                            break; // fall through to normal binding
                        }
                        result->group_by.push_back(
                            BindExpression(*stmt.select_list[sl_idx], context));
                        resolved = true;
                        break;
                    }
                }
            }
        }
        if (!resolved) {
            result->group_by.push_back(BindExpression(*expr, context));
        }
        result->has_aggregation = true;
    }

    // GROUP BY ALL: derive group keys from non-aggregate, non-window
    // entries in the SELECT list. Top-level aggregate-function detection
    // only — nested aggregates like `sum(x) + 1` are not yet handled.
    if (stmt.group_by_all && result->group_by.empty()) {
        for (size_t i = 0; i < stmt.select_list.size() && i < result->select_list.size(); i++) {
            const auto &bound = result->select_list[i];
            bool is_aggregated = false;
            if (bound->GetExpressionType() == BoundExpressionType::FUNCTION) {
                auto *fn = static_cast<const BoundFunction *>(bound.get());
                if (fn->is_aggregate) is_aggregated = true;
            }
            if (bound->GetExpressionType() == BoundExpressionType::WINDOW) {
                is_aggregated = true;
            }
            if (is_aggregated) continue;
            result->group_by.push_back(BindExpression(*stmt.select_list[i], context));
        }
        if (!result->group_by.empty()) result->has_aggregation = true;
    }

    // Bind HAVING. Populate the alias map so column refs inside HAVING
    // (e.g. `HAVING c > 100000` where c is an alias for COUNT(*)) re-bind
    // to the matching select-list expression. The map is cleared after
    // binding so other clauses get default column-resolution.
    if (stmt.having_clause) {
        context.select_list_aliases.clear();
        for (idx_t i = 0; i < result->result_names.size() &&
                          i < stmt.select_list.size(); i++) {
            // Skip STAR entries: re-binding a STAR throws. With multiple
            // expanded names mapping to a single STAR, we'd also pick the
            // wrong one. Plain column refs match the source schema directly,
            // so we don't need an alias entry for them either.
            if (stmt.select_list[i]->GetExpressionType() == ExpressionType::STAR) continue;
            if (stmt.select_list[i]->GetExpressionType() == ExpressionType::COLUMN_REF) continue;
            auto key = StringUtil::Upper(result->result_names[i]);
            // First-wins: a later select-list entry with the same alias
            // would shadow the first; SQL doesn't define HAVING in that
            // case so we just keep the first.
            if (!context.select_list_aliases.count(key))
                context.select_list_aliases[key] = stmt.select_list[i].get();
        }
        result->having_clause = BindExpression(*stmt.having_clause, context);
        context.select_list_aliases.clear();
        // HAVING implies aggregation even when there's no GROUP BY and
        // no aggregate in the SELECT list:
        //   SELECT 1 FROM t HAVING COUNT(*) > 100
        // Without this, the planner's HAVING-emit block at
        // planner.cpp:583-609 is gated on has_aggregation and silently
        // drops the predicate, returning every base-table row instead
        // of the SQL-standard implicit single-group result.
        result->has_aggregation = true;
    }

    // Bind QUALIFY.
    if (stmt.qualify_clause) {
        result->qualify_clause = BindExpression(*stmt.qualify_clause, context);
        result->has_window = true;
    }

    // Bind ORDER BY - resolve positional ordinals + select-list aliases.
    for (auto &item : stmt.order_by) {
        BoundOrderBy bound_item;
        bool resolved = false;
        // Positional ordinal: ORDER BY <integer-literal> binds to
        // select_list[N-1] (1-based), mirroring the GROUP BY ordinal
        // path above. Required so `ORDER BY 2 DESC`
        // sorts by COUNT(*), not by the constant 2. Out-of-range
        // ordinals raise BinderException.
        if (item.expression->GetExpressionType() == ExpressionType::CONSTANT) {
            auto bound_const = BindExpression(*item.expression, context);
            if (bound_const->GetExpressionType() == BoundExpressionType::CONSTANT) {
                auto &val = static_cast<BoundConstant &>(*bound_const).value;
                int64_t ord = -1;
                bool is_int_ordinal = false;
                if (val.type().id() == LogicalTypeId::INTEGER) {
                    ord = val.GetValue<int32_t>();
                    is_int_ordinal = true;
                } else if (val.type().id() == LogicalTypeId::BIGINT) {
                    ord = val.GetValue<int64_t>();
                    is_int_ordinal = true;
                }
                if (is_int_ordinal) {
                    // Reference the post-expansion select_list so
                    // `SELECT * FROM t ORDER BY 2` works (previously
                    // raised "out of range" because stmt.select_list
                    // size 1 (STAR) was used instead of the expanded
                    // result_names size N).
                    if (ord >= 1 &&
                        ord <= static_cast<int64_t>(result->select_list.size())) {
                        idx_t sl_idx = static_cast<idx_t>(ord - 1);
                        if (sl_idx < stmt.select_list.size() &&
                            stmt.select_list[sl_idx]->GetExpressionType()
                                != ExpressionType::STAR) {
                            bound_item.expression = BindExpression(
                                *stmt.select_list[sl_idx], context);
                            resolved = true;
                        } else if (sl_idx < result->result_names.size()) {
                            // Star-expanded column: re-resolve by name.
                            auto col = std::make_unique<ColumnRefExpression>(
                                result->result_names[sl_idx]);
                            try {
                                bound_item.expression = BindExpression(*col, context);
                                resolved = true;
                            } catch (...) {
                                // Fall through to alias path.
                            }
                        }
                    } else {
                        throw BinderException(
                            ErrorCode::OUT_OF_RANGE,
                            "ORDER BY position " + std::to_string(ord) +
                            " is out of range (select list has " +
                            std::to_string(result->select_list.size()) +
                            " entries)");
                    }
                }
            }
        }
        // Check if this ORDER BY expression is a select-list alias.
        // The alias-resolution branch re-binds the ORIGINAL (unbound)
        // select_list expression; that only works when select_list[i] is
        // itself a scalar. With SELECT *, result_names comes from star
        // expansion but select_list[0] is still an unbound STAR - re-
        // binding it as a scalar throws "Unhandled expression type in
        // binder". Skip alias-resolution for any index whose select_list
        // entry is a star; fall through to normal column-name resolution.
        if (!resolved && item.expression->GetExpressionType() == ExpressionType::COLUMN_REF) {
            auto &col_ref = static_cast<ColumnRefExpression &>(*item.expression);
            if (col_ref.table_name.empty()) {
                auto ref_upper = StringUtil::Upper(col_ref.column_name);
                for (idx_t i = 0; i < result->result_names.size(); i++) {
                    if (StringUtil::Upper(result->result_names[i]) == ref_upper) {
                        // Map back to the select_list entry that produced
                        // this result-name. With a bare STAR at index 0,
                        // every expanded name maps to select_list[0] -
                        // which is the star itself. Guard against that.
                        idx_t sl_idx = std::min<idx_t>(i, stmt.select_list.size() - 1);
                        if (stmt.select_list[sl_idx]->GetExpressionType()
                                == ExpressionType::STAR) {
                            break; // fall through to generic binding
                        }
                        // Re-bind the original select-list expression.
                        bound_item.expression = BindExpression(*stmt.select_list[sl_idx], context);
                        resolved = true;
                        break;
                    }
                }
            }
        }
        if (!resolved) {
            bound_item.expression = BindExpression(*item.expression, context);
        }
        bound_item.ascending = item.ascending;
        bound_item.nulls_first = item.nulls_first;
        result->order_by.push_back(std::move(bound_item));
    }

    // Bind LIMIT/OFFSET. NULL operand means "no limit" / "no offset"
    // per PostgreSQL/SQLite — defaults stay (-1 / 0). Previously the
    // GetValue<T> call on a NULL Value threw "Cannot get value from
    // NULL", crashing every parameterised pagination query.
    if (stmt.limit) {
        auto bound = BindExpression(*stmt.limit, context);
        if (bound->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &val = static_cast<BoundConstant &>(*bound).value;
            if (!val.IsNull()) {
                if (val.type().id() == LogicalTypeId::INTEGER) {
                    result->limit_count = val.GetValue<int32_t>();
                } else {
                    result->limit_count = val.GetValue<int64_t>();
                }
            }
        }
    }
    if (stmt.offset) {
        auto bound = BindExpression(*stmt.offset, context);
        if (bound->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &val = static_cast<BoundConstant &>(*bound).value;
            if (!val.IsNull()) {
                if (val.type().id() == LogicalTypeId::INTEGER) {
                    result->offset_count = val.GetValue<int32_t>();
                } else {
                    result->offset_count = val.GetValue<int64_t>();
                }
            }
        }
    }

    return result;
}

// ============================================================================
// Bind CREATE TABLE
// ============================================================================

BoundStmtPtr Binder::BindCreateTable(const CreateTableStatement &stmt) {
    auto result = std::make_unique<BoundCreateTableStatement>();
    result->table_name = stmt.table_name;
    result->if_not_exists = stmt.if_not_exists;

    for (auto &col : stmt.columns) {
        auto type = ResolveTypeName(col.type_name);
        result->columns.emplace_back(col.name, type, col.not_null);
    }
    return result;
}

// ============================================================================
// Bind DROP TABLE
// ============================================================================

BoundStmtPtr Binder::BindDropTable(const DropTableStatement &stmt) {
    auto result = std::make_unique<BoundDropTableStatement>();
    result->table_name = stmt.table_name;
    result->if_exists = stmt.if_exists;

    if (!stmt.if_exists && !catalog_.GetTable(stmt.table_name)) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + stmt.table_name + "' does not exist");
    }
    return result;
}

// ============================================================================
// Bind INSERT
// ============================================================================

BoundStmtPtr Binder::BindInsert(const InsertStatement &stmt) {
    auto result = std::make_unique<BoundInsertStatement>();

    auto *entry = catalog_.GetTable(stmt.table_name);
    if (!entry) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + stmt.table_name + "' not found");
    }
    result->table = entry;

    BindContext context;
    auto col_types = entry->GetTypes();
    // Wrap each bound value in a BoundCast when the source type
    // doesn't match the target column. Required for VARCHAR -> DATE /
    // TIMESTAMP literals and for type-widening (SMALLINT -> INT, etc).
    auto wrap_for_target = [&](BoundExprPtr bound, idx_t target) -> BoundExprPtr {
        if (target >= col_types.size()) return bound;
        auto src_id = bound->GetReturnType().id();
        auto dst_id = col_types[target].id();
        if (src_id != dst_id && src_id != LogicalTypeId::SQLNULL) {
            bound = std::make_unique<BoundCast>(std::move(bound), col_types[target]);
        }
        return bound;
    };
    // Resolve the column-name list once, validating each name and
    // detecting duplicates. When the user omits the column list, the
    // default mapping is positional (column 0..N-1 in declared order).
    std::vector<idx_t> target_indices;
    if (!stmt.column_names.empty()) {
        target_indices.reserve(stmt.column_names.size());
        std::vector<bool> seen(col_types.size(), false);
        for (auto &col_name : stmt.column_names) {
            idx_t idx = entry->GetColumnIndex(col_name);
            if (idx == INVALID_INDEX) {
                throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                    "Column '" + col_name + "' not found in table '" +
                    stmt.table_name + "'");
            }
            if (seen[idx]) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "Column '" + col_name + "' specified more than once "
                    "in INSERT column list");
            }
            seen[idx] = true;
            target_indices.push_back(idx);
        }
    }
    auto &table_cols = entry->GetColumns();
    // Bind-time NULL detection: peel BoundCast wrappers (added by
    // wrap_for_target) so CAST(NULL AS INT) and similar are also
    // caught. Without the peel, `INSERT INTO nn VALUES (CAST(NULL
    // AS INT))` silently accepted NULL into a NOT NULL column.
    auto check_not_null = [&](const BoundExpression &val, idx_t col) {
        if (col >= table_cols.size() || !table_cols[col].not_null) return;
        const BoundExpression *cur = &val;
        while (cur && cur->GetExpressionType() == BoundExpressionType::CAST) {
            cur = static_cast<const BoundCast &>(*cur).child.get();
        }
        if (!cur || cur->GetExpressionType() != BoundExpressionType::CONSTANT) return;
        auto &bc = static_cast<const BoundConstant &>(*cur);
        if (bc.value.IsNull()) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "NOT NULL constraint violation: column '" +
                table_cols[col].name + "'");
        }
    };
    for (auto &row : stmt.values) {
        if (!target_indices.empty()) {
            // Permute: bound value at user position i lands at table
            // column target_indices[i]. Unspecified columns default to
            // typed NULL (matches SQL standard implicit-default-NULL).
            if (row.size() != target_indices.size()) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "INSERT value count (" + std::to_string(row.size()) +
                    ") does not match column list count (" +
                    std::to_string(target_indices.size()) + ")");
            }
            std::vector<BoundExprPtr> permuted(col_types.size());
            for (idx_t i = 0; i < row.size(); i++) {
                auto bound = BindExpression(*row[i], context);
                idx_t tgt = target_indices[i];
                permuted[tgt] = wrap_for_target(std::move(bound), tgt);
                check_not_null(*permuted[tgt], tgt);
            }
            // Fill missing slots with NULL constants of the column's type.
            for (idx_t c = 0; c < col_types.size(); c++) {
                if (!permuted[c]) {
                    if (c < table_cols.size() && table_cols[c].not_null) {
                        throw BinderException(ErrorCode::TYPE_MISMATCH,
                            "NOT NULL constraint violation: column '" +
                            table_cols[c].name +
                            "' has no value in INSERT");
                    }
                    permuted[c] = std::make_unique<BoundConstant>(Value());
                }
            }
            result->values.push_back(std::move(permuted));
        } else {
            // Positional path: row[c] -> column c. Validate arity.
            if (row.size() != col_types.size()) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    "INSERT value count (" + std::to_string(row.size()) +
                    ") does not match column count (" +
                    std::to_string(col_types.size()) + ")");
            }
            std::vector<BoundExprPtr> bound_row;
            bound_row.reserve(row.size());
            for (idx_t c = 0; c < row.size(); c++) {
                auto bound = BindExpression(*row[c], context);
                bound_row.push_back(wrap_for_target(std::move(bound), c));
                check_not_null(*bound_row.back(), c);
            }
            result->values.push_back(std::move(bound_row));
        }
    }
    return result;
}

// ============================================================================
// Bind UPDATE
// ============================================================================

BoundStmtPtr Binder::BindUpdate(const UpdateStatement &stmt) {
    auto result = std::make_unique<BoundUpdateStatement>();
    auto *entry = catalog_.GetTable(stmt.table_name);
    if (!entry) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + stmt.table_name + "' not found");
    }
    result->table = entry;

    BindContext context;
    context.AddTable(stmt.table_name, entry);

    auto col_types = entry->GetTypes();
    for (auto &assign : stmt.assignments) {
        auto idx = entry->GetColumnIndex(assign.column_name);
        if (idx == INVALID_INDEX) {
            throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                                   "Column '" + assign.column_name + "' not found");
        }
        BoundUpdateAssignment ba;
        ba.column_index = idx;
        ba.value = BindExpression(*assign.value, context);
        if (ContainsAggregate(*ba.value)) {
            throw BinderException(ErrorCode::AGGREGATE_IN_WHERE,
                "aggregate functions are not allowed in UPDATE assignment");
        }
        // Wrap the RHS in a BoundCast when its bound type doesn't
        // match the target column type. Mirrors the INSERT VALUES
        // fix (fe38e2d): previously UPDATE silently bit-wrapped a
        // BIGINT into an INTEGER column (9999999999 -> 1410065407)
        // or wrote a VARCHAR-typed Value into a DATE slot which
        // read back as epoch.
        if (idx < col_types.size()) {
            auto src_id = ba.value->GetReturnType().id();
            auto dst_id = col_types[idx].id();
            if (src_id != dst_id && src_id != LogicalTypeId::SQLNULL) {
                ba.value = std::make_unique<BoundCast>(
                    std::move(ba.value), col_types[idx]);
            }
        }
        // NOT NULL constraint: reject literal NULL into a NOT NULL
        // column at bind time. Peel BoundCast wrappers so CAST(NULL
        // AS INT) is also caught. Computed NULLs at runtime are not
        // caught here (deferred to executor-side enforcement).
        auto &upd_cols = entry->GetColumns();
        if (idx < upd_cols.size() && upd_cols[idx].not_null) {
            const BoundExpression *cur = ba.value.get();
            while (cur && cur->GetExpressionType() == BoundExpressionType::CAST) {
                cur = static_cast<const BoundCast &>(*cur).child.get();
            }
            if (cur && cur->GetExpressionType() == BoundExpressionType::CONSTANT) {
                auto &bc = static_cast<const BoundConstant &>(*cur);
                if (bc.value.IsNull()) {
                    throw BinderException(ErrorCode::TYPE_MISMATCH,
                        "NOT NULL constraint violation: column '" +
                        upd_cols[idx].name + "'");
                }
            }
        }
        result->assignments.push_back(std::move(ba));
    }

    if (stmt.where_clause) {
        result->where_clause = BindExpression(*stmt.where_clause, context);
        if (ContainsAggregate(*result->where_clause)) {
            throw BinderException(ErrorCode::AGGREGATE_IN_WHERE,
                "aggregate functions are not allowed in WHERE clause");
        }
    }

    return result;
}

// ============================================================================
// Bind DELETE
// ============================================================================

BoundStmtPtr Binder::BindDelete(const DeleteStatement &stmt) {
    auto result = std::make_unique<BoundDeleteStatement>();
    auto *entry = catalog_.GetTable(stmt.table_name);
    if (!entry) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + stmt.table_name + "' not found");
    }
    result->table = entry;

    BindContext context;
    context.AddTable(stmt.table_name, entry);

    if (stmt.where_clause) {
        result->where_clause = BindExpression(*stmt.where_clause, context);
        if (ContainsAggregate(*result->where_clause)) {
            throw BinderException(ErrorCode::AGGREGATE_IN_WHERE,
                "aggregate functions are not allowed in WHERE clause");
        }
    }

    return result;
}

// ============================================================================
// Bind FROM
// ============================================================================

void Binder::BindTableRef(const TableRef &ref, BindContext &context) {
    auto *entry = catalog_.GetTable(ref.table_name);
    if (!entry) {
        throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                               "Table '" + ref.table_name + "' not found");
    }
    auto alias = ref.alias.empty() ? ref.table_name : ref.alias;
    context.AddTable(alias, entry);

    // Recursively bind every joined table on the chain. Previously this
    // only handled the immediate right side, so `FROM a, b, c` (SQL-92
    // comma-FROM) or `a JOIN b JOIN c` only ever added a + b, and any
    // column ref into c failed at bind. Recursion makes the chain length
    // unbounded.
    if (ref.right) {
        BindTableRef(*ref.right, context);
    }
}

// ============================================================================
// Bind expressions
// ============================================================================

BoundExprPtr Binder::BindExpression(const ParsedExpression &expr, BindContext &context) {
    switch (expr.GetExpressionType()) {
    case ExpressionType::COLUMN_REF:
        return BindColumnRef(static_cast<const ColumnRefExpression &>(expr), context);
    case ExpressionType::CONSTANT:
        return BindConstant(static_cast<const ConstantExpression &>(expr));
    case ExpressionType::COMPARISON:
        return BindComparison(static_cast<const ComparisonExpression &>(expr), context);
    case ExpressionType::CONJUNCTION:
        return BindConjunction(static_cast<const ConjunctionExpression &>(expr), context);
    case ExpressionType::NEGATION:
        return BindNegation(static_cast<const NegationExpression &>(expr), context);
    case ExpressionType::IS_NULL:
        return BindIsNull(static_cast<const IsNullExpression &>(expr), context);
    case ExpressionType::IS_BOOL:
        return BindIsBool(static_cast<const IsBoolExpression &>(expr), context);
    case ExpressionType::ARITHMETIC:
        return BindArithmetic(static_cast<const ArithmeticExpression &>(expr), context);
    case ExpressionType::UNARY_MINUS:
        return BindUnaryMinus(static_cast<const UnaryMinusExpression &>(expr), context);
    case ExpressionType::FUNCTION:
        return BindFunction(static_cast<const FunctionExpression &>(expr), context);
    case ExpressionType::CAST:
        return BindCast(static_cast<const CastExpression &>(expr), context);
    case ExpressionType::WINDOW:
        return BindWindow(static_cast<const WindowExpression &>(expr), context);
    case ExpressionType::SUBQUERY:
        return BindSubquery(static_cast<const SubqueryExpression &>(expr), context);
    default:
        throw InternalException("Unhandled expression type in binder");
    }
}

BoundExprPtr Binder::BindColumnRef(const ColumnRefExpression &expr, BindContext &context) {
    // SELECT-list alias resolution (HAVING): when the active context has
    // an alias map populated, an unqualified column ref whose name matches
    // a select-list alias re-binds the original select-list expression.
    // Required for SQL forms like `... COUNT(*) AS c ... HAVING c > N`.
    if (expr.table_name.empty() && !context.select_list_aliases.empty()) {
        auto it = context.select_list_aliases.find(StringUtil::Upper(expr.column_name));
        if (it != context.select_list_aliases.end() && it->second) {
            return BindExpression(*it->second, context);
        }
    }
    auto [table, combined_idx] = context.ResolveColumn(expr.column_name, expr.table_name);
    idx_t offset = context.GetTableOffset(table);
    idx_t local_idx = combined_idx - offset;
    auto type = context.tables.at(table)->GetColumns()[local_idx].type;
    return std::make_unique<BoundColumnRef>(expr.column_name, combined_idx, type);
}

BoundExprPtr Binder::BindConstant(const ConstantExpression &expr) {
    if (expr.is_null) {
        return std::make_unique<BoundConstant>(Value());
    }

    switch (expr.literal_type) {
    case TokenType::INTEGER_LITERAL: {
        // Out-of-range integer literals previously leaked raw
        // std::out_of_range from std::stoll up to the shell as
        // "Error: stoll argument out of range". Promote to a classified
        // ParserException. INT64_MAX+1 and beyond falls here; INT64_MIN
        // is parsed as NEG(INT64_MAX+1) by the unary-minus path which
        // means -9223372036854775808 still hits this branch (the
        // absolute value overflows by one) — documented limitation.
        int64_t val;
        try {
            val = std::stoll(expr.value);
        } catch (const std::out_of_range &) {
            throw ParserException(
                "Numeric literal '" + expr.value +
                "' exceeds BIGINT range");
        } catch (const std::invalid_argument &) {
            throw ParserException(
                "Could not parse numeric literal '" + expr.value + "'");
        }
        if (val >= INT32_MIN && val <= INT32_MAX) {
            return std::make_unique<BoundConstant>(Value::INTEGER(static_cast<int32_t>(val)));
        }
        return std::make_unique<BoundConstant>(Value::BIGINT(val));
    }
    case TokenType::FLOAT_LITERAL: {
        double d;
        try {
            d = std::stod(expr.value);
        } catch (const std::out_of_range &) {
            throw ParserException(
                "Floating-point literal '" + expr.value +
                "' exceeds DOUBLE range");
        } catch (const std::invalid_argument &) {
            throw ParserException(
                "Could not parse floating-point literal '" + expr.value + "'");
        }
        return std::make_unique<BoundConstant>(Value::DOUBLE(d));
    }
    case TokenType::STRING_LITERAL:
        return std::make_unique<BoundConstant>(Value::VARCHAR(expr.value));
    case TokenType::KW_TRUE:
        return std::make_unique<BoundConstant>(Value::BOOLEAN(true));
    case TokenType::KW_FALSE:
        return std::make_unique<BoundConstant>(Value::BOOLEAN(false));
    case TokenType::KW_DATE: {
        // SQL-92 typed literal DATE 'YYYY-MM-DD' — parsed at the
        // expression-grammar level so the binder receives a string
        // tagged with the DATE token; convert via the existing helper.
        int32_t days;
        if (!Value::TryParseDateStringEpochDays(expr.value.data(), expr.value.size(), days)) {
            throw ParserException("Invalid DATE literal '" + expr.value + "'");
        }
        return std::make_unique<BoundConstant>(Value::DATE(days));
    }
    case TokenType::KW_TIMESTAMP: {
        int64_t micros;
        if (!Value::TryParseTimestampMicros(expr.value.data(), expr.value.size(), micros)) {
            int32_t days;
            if (!Value::TryParseDateStringEpochDays(expr.value.data(), expr.value.size(), days)) {
                throw ParserException("Invalid TIMESTAMP literal '" + expr.value + "'");
            }
            micros = static_cast<int64_t>(days) * 86400LL * 1000000LL;
        }
        return std::make_unique<BoundConstant>(Value::TIMESTAMP(micros));
    }
    case TokenType::KW_TIME:
        throw NotImplementedException("TIME literal not yet supported");
    default:
        throw InternalException("Unknown constant literal type");
    }
}

BoundExprPtr Binder::BindComparison(const ComparisonExpression &expr, BindContext &context) {
    auto left = BindExpression(*expr.left, context);
    auto right = BindExpression(*expr.right, context);

    // Promote a literal constant to the column's type when one side is a
    // column reference and the other is a CONSTANT of a compatible numeric
    // type. Without this the executor falls to a string-stod-per-row slow
    // path on `WHERE bigint_col > 50` (50 binds as INTEGER, the column is
    // BIGINT, types differ, so the typed-compare branch is skipped). This
    // changes a 2.5-second filter scan over 10M rows into a ~30 ms one.
    auto promote_literal = [](BoundExpression &target_typed, BoundExpression *literal) {
        if (literal->GetExpressionType() != BoundExpressionType::CONSTANT) return;
        auto &konst = *static_cast<BoundConstant *>(literal);
        if (konst.value.IsNull()) return;
        auto src = konst.value.type().id();
        auto dst = target_typed.GetReturnType().id();
        if (src == dst) return;
        auto is_int = [](LogicalTypeId t) {
            return t == LogicalTypeId::TINYINT || t == LogicalTypeId::SMALLINT ||
                   t == LogicalTypeId::INTEGER || t == LogicalTypeId::BIGINT;
        };
        if (!is_int(src) || !is_int(dst)) return;
        int64_t v = 0;
        switch (src) {
        case LogicalTypeId::TINYINT:  v = konst.value.GetValue<int8_t>(); break;
        case LogicalTypeId::SMALLINT: v = konst.value.GetValue<int16_t>(); break;
        case LogicalTypeId::INTEGER:  v = konst.value.GetValue<int32_t>(); break;
        case LogicalTypeId::BIGINT:   v = konst.value.GetValue<int64_t>(); break;
        default: return;
        }
        switch (dst) {
        case LogicalTypeId::TINYINT:
            if (v < INT8_MIN || v > INT8_MAX) return;
            konst.value = Value::TINYINT(static_cast<int8_t>(v)); break;
        case LogicalTypeId::SMALLINT:
            if (v < INT16_MIN || v > INT16_MAX) return;
            konst.value = Value::SMALLINT(static_cast<int16_t>(v)); break;
        case LogicalTypeId::INTEGER:
            if (v < INT32_MIN || v > INT32_MAX) return;
            konst.value = Value::INTEGER(static_cast<int32_t>(v)); break;
        case LogicalTypeId::BIGINT:
            konst.value = Value::BIGINT(v); break;
        default: return;
        }
        konst.SetReturnType(target_typed.GetReturnType());
    };
    // Coerce a strict 'YYYY-MM-DD' VARCHAR literal against an integer date
    // column (USMALLINT/INTEGER days-since-epoch, e.g. a packed date column)
    // so the typed-compare fast path fires instead of falling to per-row
    // string-stod. Date-literal filter queries land here.
    auto promote_date_literal = [](BoundExpression &target_typed, BoundExpression *literal) {
        if (literal->GetExpressionType() != BoundExpressionType::CONSTANT) return;
        auto &konst = *static_cast<BoundConstant *>(literal);
        if (konst.value.IsNull()) return;
        if (konst.value.type().id() != LogicalTypeId::VARCHAR) return;
        auto dst = target_typed.GetReturnType().id();
        if (dst != LogicalTypeId::USMALLINT && dst != LogicalTypeId::SMALLINT &&
            dst != LogicalTypeId::INTEGER && dst != LogicalTypeId::BIGINT) return;
        const std::string &s = konst.value.GetValue<std::string>();
        int32_t days = 0;
        if (!Value::TryParseDateStringEpochDays(s.data(), s.size(), days)) return;
        switch (dst) {
        case LogicalTypeId::USMALLINT:
            if (days < 0 || days > UINT16_MAX) return;
            konst.value = Value::USMALLINT(static_cast<uint16_t>(days)); break;
        case LogicalTypeId::SMALLINT:
            if (days < INT16_MIN || days > INT16_MAX) return;
            konst.value = Value::SMALLINT(static_cast<int16_t>(days)); break;
        case LogicalTypeId::INTEGER:
            konst.value = Value::INTEGER(days); break;
        case LogicalTypeId::BIGINT:
            konst.value = Value::BIGINT(days); break;
        default: return;
        }
        konst.SetReturnType(target_typed.GetReturnType());
    };
    if (left->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        promote_date_literal(*left, right.get());
        promote_literal(*left, right.get());
    } else if (right->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        promote_date_literal(*right, left.get());
        promote_literal(*right, left.get());
    }

    return std::make_unique<BoundComparison>(expr.op, std::move(left), std::move(right));
}

BoundExprPtr Binder::BindConjunction(const ConjunctionExpression &expr, BindContext &context) {
    auto left = BindExpression(*expr.left, context);
    auto right = BindExpression(*expr.right, context);
    return std::make_unique<BoundConjunction>(expr.op, std::move(left), std::move(right));
}

BoundExprPtr Binder::BindNegation(const NegationExpression &expr, BindContext &context) {
    auto child = BindExpression(*expr.child, context);
    return std::make_unique<BoundNegation>(std::move(child));
}

BoundExprPtr Binder::BindIsNull(const IsNullExpression &expr, BindContext &context) {
    auto child = BindExpression(*expr.child, context);
    return std::make_unique<BoundIsNull>(std::move(child), expr.is_not);
}

BoundExprPtr Binder::BindIsBool(const IsBoolExpression &expr, BindContext &context) {
    auto child = BindExpression(*expr.child, context);
    auto child_type = child->GetReturnType().id();
    BoundIsBool::Predicate p = BoundIsBool::Predicate::TRUE_;
    switch (expr.pred) {
        case IsBoolExpression::Predicate::TRUE_:    p = BoundIsBool::Predicate::TRUE_; break;
        case IsBoolExpression::Predicate::FALSE_:   p = BoundIsBool::Predicate::FALSE_; break;
        case IsBoolExpression::Predicate::UNKNOWN_: p = BoundIsBool::Predicate::UNKNOWN_; break;
    }
    // IS UNKNOWN accepts any operand (it's equivalent to IS NULL). The
    // TRUE/FALSE forms require a BOOLEAN operand per SQL standard, but
    // we accept SQLNULL too (NULL IS TRUE is FALSE, NULL IS FALSE is FALSE).
    if (p != BoundIsBool::Predicate::UNKNOWN_) {
        if (child_type != LogicalTypeId::BOOLEAN &&
            child_type != LogicalTypeId::SQLNULL) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "IS [NOT] TRUE/FALSE requires a boolean operand, got " +
                child->GetReturnType().ToString());
        }
    }
    return std::make_unique<BoundIsBool>(std::move(child), p, expr.is_not);
}

BoundExprPtr Binder::BindArithmetic(const ArithmeticExpression &expr, BindContext &context) {
    auto left = BindExpression(*expr.left, context);
    auto right = BindExpression(*expr.right, context);
    // || is string concatenation.
    if (expr.op == "||") {
        return std::make_unique<BoundArithmetic>(expr.op, std::move(left), std::move(right),
                                                  LogicalType::VARCHAR());
    }
    // DATE arithmetic per SQL standard:
    //   DATE + integer  -> DATE   (commutative)
    //   DATE - integer  -> DATE   (DATE on left only)
    //   DATE - DATE     -> INTEGER (days between)
    //   DATE * / / etc. -> error
    // Without this, every DATE math expression returned raw epoch days
    // (DATE '2024-01-01' + 5 -> 19728 instead of 2024-01-06) because
    // ResolveArithmeticType fell through to the INTEGER branch.
    auto left_id = left->GetReturnType().id();
    auto right_id = right->GetReturnType().id();
    bool left_date  = (left_id  == LogicalTypeId::DATE);
    bool right_date = (right_id == LogicalTypeId::DATE);
    auto is_int_id = [](LogicalTypeId t) {
        return t == LogicalTypeId::TINYINT  || t == LogicalTypeId::SMALLINT ||
               t == LogicalTypeId::INTEGER  || t == LogicalTypeId::BIGINT;
    };
    if (left_date || right_date) {
        if (expr.op == "+") {
            // DATE + INT or INT + DATE
            if (left_date && (is_int_id(right_id) || right_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::DATE());
            }
            if (right_date && (is_int_id(left_id) || left_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::DATE());
            }
        } else if (expr.op == "-") {
            // DATE - INT -> DATE; DATE - DATE -> INTEGER (days).
            if (left_date && right_date) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::INTEGER());
            }
            if (left_date && (is_int_id(right_id) || right_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::DATE());
            }
        }
        // Any other DATE-involving combination is a type error.
        throw BinderException(ErrorCode::TYPE_MISMATCH,
            "Invalid DATE arithmetic: " + left->GetReturnType().ToString() +
            " " + expr.op + " " + right->GetReturnType().ToString());
    }
    // TIMESTAMP arithmetic: integer operand is seconds.
    //   TIMESTAMP + N -> TIMESTAMP   (commutative)
    //   TIMESTAMP - N -> TIMESTAMP   (TIMESTAMP on left only)
    //   TIMESTAMP - TIMESTAMP -> BIGINT (microseconds difference)
    // Result type only — the executor needs to scale the integer
    // operand by 1e6 (sec->micros) before the int64 add/sub; without
    // result-type tagging here, the integer is added to a TIMESTAMP
    // (int64 micros) and the result is rendered as raw INTEGER bytes.
    bool left_ts  = (left_id  == LogicalTypeId::TIMESTAMP || left_id == LogicalTypeId::TIMESTAMP_TZ);
    bool right_ts = (right_id == LogicalTypeId::TIMESTAMP || right_id == LogicalTypeId::TIMESTAMP_TZ);
    if (left_ts || right_ts) {
        auto pick_ts = [&]() -> LogicalType {
            return LogicalType::TIMESTAMP();
        };
        if (expr.op == "+") {
            if (left_ts && (is_int_id(right_id) || right_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), pick_ts());
            }
            if (right_ts && (is_int_id(left_id) || left_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), pick_ts());
            }
        } else if (expr.op == "-") {
            if (left_ts && right_ts) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), LogicalType::BIGINT());
            }
            if (left_ts && (is_int_id(right_id) || right_id == LogicalTypeId::SQLNULL)) {
                return std::make_unique<BoundArithmetic>(
                    expr.op, std::move(left), std::move(right), pick_ts());
            }
        }
        throw BinderException(ErrorCode::TYPE_MISMATCH,
            "Invalid TIMESTAMP arithmetic: " + left->GetReturnType().ToString() +
            " " + expr.op + " " + right->GetReturnType().ToString());
    }
    auto result_type = ResolveArithmeticType(left->GetReturnType(), right->GetReturnType());
    return std::make_unique<BoundArithmetic>(expr.op, std::move(left), std::move(right),
                                             result_type);
}

BoundExprPtr Binder::BindUnaryMinus(const UnaryMinusExpression &expr, BindContext &context) {
    auto child = BindExpression(*expr.child, context);
    return std::make_unique<BoundUnaryMinus>(std::move(child));
}

BoundExprPtr Binder::BindFunction(const FunctionExpression &expr, BindContext &context) {
    auto name = StringUtil::Upper(expr.function_name);
    // Canonical alias normalization — collapse SQL-standard synonyms
    // onto SlothDB's primary names so the binder/executor only need to
    // recognise one spelling. byte-length semantics (slothdb's LENGTH
    // returns bytes today, matching the byte-oriented LEFT/RIGHT/
    // SUBSTRING family); true UTF-8 codepoint length is a separate
    // future change.
    if (name == "CHAR_LENGTH" || name == "CHARACTER_LENGTH") {
        name = "LENGTH";
    }
    bool is_agg = IsAggregateFunction(name);

    std::vector<BoundExprPtr> args;
    for (auto &arg : expr.arguments) {
        args.push_back(BindExpression(*arg, context));
    }

    // Validate aggregate arity. SQL standard: most aggregates take
    // exactly one operand. COUNT additionally allows zero args via
    // the COUNT(*) form (parser sets expr.is_star). Without these
    // checks, COUNT(a,b) silently returned COUNT(a) — silent wrong
    // result that hid data-quality issues — and MIN() / SUM() with
    // zero args silently returned NULL/0.
    if (is_agg) {
        size_t n = args.size();
        auto need = [&](size_t lo, size_t hi) {
            if (n < lo || n > hi) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    name + " expects " +
                    (lo == hi ? std::to_string(lo)
                              : std::to_string(lo) + "-" + std::to_string(hi)) +
                    " argument(s), got " + std::to_string(n));
            }
        };
        if (name == "COUNT") {
            if (!expr.is_star) need(1, 1);
            // else 0 args is fine
        } else if (name == "STRING_AGG" || name == "LISTAGG" ||
                   name == "GROUP_CONCAT") {
            need(1, 2);
        } else if (name == "BOOL_AND" || name == "BOOL_OR" ||
                   name == "MIN" || name == "MAX" ||
                   name == "SUM" || name == "AVG" ||
                   name == "MEDIAN" ||
                   name == "STDDEV" || name == "STDDEV_SAMP" ||
                   name == "STDDEV_POP" || name == "VARIANCE" ||
                   name == "VAR_SAMP" || name == "VAR_POP") {
            need(1, 1);
        }
    }

    // Determine return type.
    LogicalType return_type = LogicalType::INTEGER(); // default
    if (name == "COUNT") {
        return_type = LogicalType::BIGINT();
    } else if (name == "SUM" || name == "AVG") {
        // SUM/AVG require a numeric operand. The accumulator is `double`
        // (and casts to BIGINT for SUM's integer types at emit). Allowing
        // VARCHAR / BOOLEAN / STRUCT / LIST inputs through has been silently
        // wrong:
        //   AVG('a','b','c')  -> 0
        //   SUM('1','2','3')  -> 0    (no string-to-number coercion happens
        //                              in the hot loop; ReadDouble returns
        //                              0.0 for VARCHAR)
        //   SUM(true,false)   -> false  (treated as bool aggregate; wrong)
        // Per SQL standard these should be type errors. Reject at bind.
        // SQLNULL is allowed (degenerate column of NULLs returns NULL/0
        // through the existing empty-set path).
        if (!args.empty()) {
            auto arg_id = args[0]->GetReturnType().id();
            bool numeric =
                arg_id == LogicalTypeId::TINYINT  || arg_id == LogicalTypeId::SMALLINT ||
                arg_id == LogicalTypeId::INTEGER  || arg_id == LogicalTypeId::BIGINT   ||
                arg_id == LogicalTypeId::HUGEINT  ||
                arg_id == LogicalTypeId::UTINYINT || arg_id == LogicalTypeId::USMALLINT ||
                arg_id == LogicalTypeId::UINTEGER || arg_id == LogicalTypeId::UBIGINT  ||
                arg_id == LogicalTypeId::FLOAT    || arg_id == LogicalTypeId::DOUBLE   ||
                arg_id == LogicalTypeId::DECIMAL  || arg_id == LogicalTypeId::SQLNULL;
            if (!numeric) {
                throw BinderException(ErrorCode::TYPE_MISMATCH,
                    name + " requires a numeric argument, got " +
                    args[0]->GetReturnType().ToString());
            }
        }
        if (name == "SUM") {
            return_type = args.empty() ? LogicalType::BIGINT() : args[0]->GetReturnType();
            // SUM of small integers widens to BIGINT to give more headroom
            // before overflow (still uses double internally — full-fidelity
            // BIGINT SUM requires an integer accumulator, deferred).
            if (return_type.id() == LogicalTypeId::INTEGER ||
                return_type.id() == LogicalTypeId::SMALLINT ||
                return_type.id() == LogicalTypeId::TINYINT) {
                return_type = LogicalType::BIGINT();
            }
        } else {
            return_type = LogicalType::DOUBLE();
        }
    } else if (name == "MIN" || name == "MAX") {
        return_type = args.empty() ? LogicalType::INTEGER() : args[0]->GetReturnType();
    }
    // Scalar function return types.
    else if (name == "LENGTH" || name == "STRLEN" || name == "ASCII" ||
             name == "OCTET_LENGTH" || name == "BIT_LENGTH") {
        return_type = LogicalType::INTEGER();
    } else if (name == "UPPER" || name == "LOWER" || name == "TRIM" ||
               name == "LTRIM" || name == "RTRIM" || name == "REPLACE" ||
               name == "SUBSTRING" || name == "SUBSTR" || name == "CONCAT" ||
               name == "CONCAT_WS" || name == "CHR" ||
               name == "HEX" || name == "TO_HEX" || name == "UNHEX" ||
               name == "MD5" || name == "SHA1" || name == "SHA_1" ||
               name == "SHA256" || name == "SHA2_256" ||
               name == "SHA512" || name == "SHA2_512" ||
               name == "BASE64_ENCODE" || name == "BASE64_DECODE" ||
               name == "TO_BASE64" || name == "FROM_BASE64") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "ABS") {
        return_type = args.empty() ? LogicalType::INTEGER() : args[0]->GetReturnType();
    } else if (name == "CEIL" || name == "CEILING" || name == "FLOOR" ||
               name == "ROUND" || name == "SQRT" || name == "CBRT" ||
               name == "POWER" || name == "MOD") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "COALESCE" || name == "IFNULL" || name == "NVL") {
        // Result type unifies across all args via SQL numeric promotion
        // (matches CASE/IF/IIF logic below). Previously this used the
        // first non-NULL-typed arg, which caused the same wrong-result
        // bugs as the pre-fix CASE branch:
        //   COALESCE(1, 2.5)                     -> 1   (DOUBLE 2.5 cast
        //                                                to INT slot)
        //   COALESCE(int_col, double_col)         -> 0   (when int_col is
        //                                                 NULL, DOUBLE
        //                                                 reads as garbage
        //                                                 from INT slot)
        //   COALESCE(int_col, 'foo')              -> int_col (varchar
        //                                                  silently dropped)
        bool any_varchar = false;
        bool any_numeric = false;
        LogicalTypeId widest_numeric = LogicalTypeId::SQLNULL;
        auto rank = [](LogicalTypeId x) -> int {
            switch (x) {
            case LogicalTypeId::DOUBLE:   return 6;
            case LogicalTypeId::FLOAT:    return 5;
            case LogicalTypeId::BIGINT:   return 4;
            case LogicalTypeId::INTEGER:  return 3;
            case LogicalTypeId::SMALLINT: return 2;
            case LogicalTypeId::TINYINT:  return 1;
            default:                      return 0;
            }
        };
        for (auto &a : args) {
            auto id = a->GetReturnType().id();
            if (id == LogicalTypeId::SQLNULL) continue;
            if (id == LogicalTypeId::VARCHAR) { any_varchar = true; continue; }
            any_numeric = true;
            if (rank(id) > rank(widest_numeric)) widest_numeric = id;
        }
        if (any_varchar) {
            return_type = LogicalType::VARCHAR();
        } else if (any_numeric) {
            switch (widest_numeric) {
            case LogicalTypeId::DOUBLE:   return_type = LogicalType::DOUBLE(); break;
            case LogicalTypeId::FLOAT:    return_type = LogicalType::DOUBLE(); break;
            case LogicalTypeId::BIGINT:   return_type = LogicalType::BIGINT(); break;
            case LogicalTypeId::INTEGER:  return_type = LogicalType::INTEGER(); break;
            case LogicalTypeId::SMALLINT: return_type = LogicalType::INTEGER(); break;
            case LogicalTypeId::TINYINT:  return_type = LogicalType::INTEGER(); break;
            default:                      return_type = LogicalType::INTEGER(); break;
            }
        } else {
            return_type = LogicalType::SQLNULL();
        }
        // Wrap mismatched args in BoundCast so the executor reads each
        // through a type-compatible buffer. SQLNULL is preserved (NULL
        // is type-agnostic — the executor uses validity).
        for (auto &a : args) {
            if (a->GetReturnType().id() != return_type.id() &&
                a->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                a = std::make_unique<BoundCast>(std::move(a), return_type);
            }
        }
    } else if (name == "NULLIF") {
        // Same unification pattern as CASE/COALESCE. Without it,
        //   NULLIF(1, 1.0) returns 1 instead of NULL — the DOUBLE 1.0
        //   was read through an INTEGER Value union slot and compared
        //   as a different value, so the equality check failed.
        bool any_varchar = false;
        bool any_numeric = false;
        LogicalTypeId widest_numeric = LogicalTypeId::SQLNULL;
        auto rank = [](LogicalTypeId x) -> int {
            switch (x) {
            case LogicalTypeId::DOUBLE:   return 6;
            case LogicalTypeId::FLOAT:    return 5;
            case LogicalTypeId::BIGINT:   return 4;
            case LogicalTypeId::INTEGER:  return 3;
            case LogicalTypeId::SMALLINT: return 2;
            case LogicalTypeId::TINYINT:  return 1;
            default:                      return 0;
            }
        };
        for (auto &a : args) {
            auto id = a->GetReturnType().id();
            if (id == LogicalTypeId::SQLNULL) continue;
            if (id == LogicalTypeId::VARCHAR) { any_varchar = true; continue; }
            any_numeric = true;
            if (rank(id) > rank(widest_numeric)) widest_numeric = id;
        }
        if (any_varchar) {
            return_type = LogicalType::VARCHAR();
        } else if (any_numeric) {
            switch (widest_numeric) {
            case LogicalTypeId::DOUBLE:   return_type = LogicalType::DOUBLE(); break;
            case LogicalTypeId::FLOAT:    return_type = LogicalType::DOUBLE(); break;
            case LogicalTypeId::BIGINT:   return_type = LogicalType::BIGINT(); break;
            case LogicalTypeId::INTEGER:  return_type = LogicalType::INTEGER(); break;
            case LogicalTypeId::SMALLINT: return_type = LogicalType::INTEGER(); break;
            case LogicalTypeId::TINYINT:  return_type = LogicalType::INTEGER(); break;
            default:                      return_type = LogicalType::INTEGER(); break;
            }
        } else {
            return_type = LogicalType::SQLNULL();
        }
        for (auto &a : args) {
            if (a->GetReturnType().id() != return_type.id() &&
                a->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                a = std::make_unique<BoundCast>(std::move(a), return_type);
            }
        }
    } else if (name == "CASE" || name == "IF" || name == "IIF") {
        // CASE(when, then, [when, then, ...] [else]) — args layout:
        //   indices 0, 2, 4, ... = WHEN predicates
        //   indices 1, 3, 5, ... = THEN expressions
        //   last arg when size is odd = ELSE expression
        // SQL standard: result type unifies all THEN + ELSE types via
        // numeric promotion (widest wins), or falls back to VARCHAR when
        // mixing string and non-string, or SQLNULL when every branch is
        // NULL. Previously the return type was hardcoded to the first
        // THEN's type, so CASE WHEN 1=2 THEN 1 ELSE 2.5 END returned 0
        // (the DOUBLE 2.5 was written into an INTEGER-typed Value union
        // slot and read back as garbage). Same pattern for any mixed
        // type branch.
        bool any_varchar = false;
        bool any_numeric = false;
        LogicalTypeId widest_numeric = LogicalTypeId::SQLNULL;
        auto rank = [](LogicalTypeId x) -> int {
            switch (x) {
            case LogicalTypeId::DOUBLE:   return 6;
            case LogicalTypeId::FLOAT:    return 5;
            case LogicalTypeId::BIGINT:   return 4;
            case LogicalTypeId::INTEGER:  return 3;
            case LogicalTypeId::SMALLINT: return 2;
            case LogicalTypeId::TINYINT:  return 1;
            default:                      return 0;
            }
        };
        auto consider = [&](const LogicalType &t) {
            auto id = t.id();
            if (id == LogicalTypeId::SQLNULL) return;
            if (id == LogicalTypeId::VARCHAR) { any_varchar = true; return; }
            any_numeric = true;
            if (rank(id) > rank(widest_numeric)) widest_numeric = id;
        };
        for (size_t i = 1; i < args.size(); i += 2) {
            consider(args[i]->GetReturnType());
        }
        if (args.size() % 2 == 1 && !args.empty()) {
            consider(args.back()->GetReturnType());
        }
        if (any_varchar) {
            return_type = LogicalType::VARCHAR();
        } else if (any_numeric) {
            switch (widest_numeric) {
            case LogicalTypeId::DOUBLE:   return_type = LogicalType::DOUBLE(); break;
            case LogicalTypeId::FLOAT:    return_type = LogicalType::DOUBLE(); break;
            case LogicalTypeId::BIGINT:   return_type = LogicalType::BIGINT(); break;
            case LogicalTypeId::INTEGER:  return_type = LogicalType::INTEGER(); break;
            case LogicalTypeId::SMALLINT: return_type = LogicalType::INTEGER(); break;
            case LogicalTypeId::TINYINT:  return_type = LogicalType::INTEGER(); break;
            default:                      return_type = LogicalType::INTEGER(); break;
            }
        } else {
            return_type = LogicalType::SQLNULL();
        }
        // Wrap mismatched THEN/ELSE in a BoundCast so the executor's
        // per-row write into the unified-type result Vector reads from
        // a compatible source. SQLNULL is preserved verbatim (NULL is
        // type-agnostic; ExpressionExecutor handles it via validity).
        for (size_t i = 1; i < args.size(); i += 2) {
            if (args[i]->GetReturnType().id() != return_type.id() &&
                args[i]->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                args[i] = std::make_unique<BoundCast>(std::move(args[i]), return_type);
            }
        }
        if (args.size() % 2 == 1 && !args.empty()) {
            auto &last = args.back();
            if (last->GetReturnType().id() != return_type.id() &&
                last->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                last = std::make_unique<BoundCast>(std::move(last), return_type);
            }
        }
    } else if (name == "IN" || name == "BETWEEN") {
        return_type = LogicalType::BOOLEAN();
    } else if (name == "NOW" || name == "CURRENT_TIMESTAMP" ||
               name == "MAKE_TIMESTAMP") {
        return_type = LogicalType::TIMESTAMP();
    } else if (name == "TO_TIMESTAMP" || name == "DATE_TRUNC") {
        return_type = LogicalType::BIGINT(); // microseconds since epoch
    } else if (name == "CURRENT_DATE") {
        return_type = LogicalType::DATE();
    } else if (name == "EXTRACT" || name == "DATE_PART") {
        return_type = LogicalType::BIGINT();
    } else if (name == "EPOCH_MS") {
        return_type = LogicalType::DOUBLE();
    }
    // Additional string functions.
    else if (name == "POSITION" || name == "STRPOS" || name == "INSTR") {
        return_type = LogicalType::INTEGER();
    } else if (name == "LEFT" || name == "RIGHT" || name == "LPAD" || name == "RPAD" ||
               name == "REVERSE" || name == "REPEAT" || name == "SPLIT_PART" ||
               name == "INITCAP") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "STARTS_WITH" || name == "ENDS_WITH" || name == "CONTAINS" ||
               name == "PREFIX" || name == "SUFFIX") {
        return_type = LogicalType::BOOLEAN();
    }
    // Additional math functions.
    else if (name == "LOG" || name == "LN" || name == "LOG2" || name == "LOG10" ||
             name == "EXP" || name == "PI" || name == "RANDOM" || name == "RAND" ||
             name == "SIN" || name == "COS" || name == "TAN" ||
             name == "ASIN" || name == "ACOS" || name == "ATAN" ||
             name == "SINH" || name == "COSH" || name == "TANH" ||
             name == "ASINH" || name == "ACOSH" || name == "ATANH") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "ISNAN" || name == "ISINF" || name == "ISFINITE") {
        return_type = LogicalType::BOOLEAN();
    } else if (name == "SIGN") {
        return_type = LogicalType::INTEGER();
    } else if (name == "ATAN2" || name == "DEGREES" || name == "RADIANS" ||
               name == "TRUNC" || name == "TRUNCATE") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "INITCAP") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "EXISTS" || name == "REGEXP_MATCHES" || name == "REGEXP_MATCH") {
        return_type = LogicalType::BOOLEAN();
    } else if (name == "REGEXP_REPLACE" || name == "REGEXP_EXTRACT") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "LEAST" || name == "GREATEST") {
        // Same numeric/string unification as COALESCE/NULLIF: walk all
        // args, widen to the highest numeric type, fall back to VARCHAR
        // when types mix, SQLNULL when every arg is NULL. Without this,
        // GREATEST(1, 2.5) returned 1 because args[0]'s INTEGER type
        // truncated the DOUBLE second arg at the result-Vector write.
        bool any_varchar = false;
        bool any_numeric = false;
        LogicalTypeId widest_numeric = LogicalTypeId::SQLNULL;
        auto rank = [](LogicalTypeId x) -> int {
            switch (x) {
            case LogicalTypeId::DOUBLE:   return 6;
            case LogicalTypeId::FLOAT:    return 5;
            case LogicalTypeId::BIGINT:   return 4;
            case LogicalTypeId::INTEGER:  return 3;
            case LogicalTypeId::SMALLINT: return 2;
            case LogicalTypeId::TINYINT:  return 1;
            default:                      return 0;
            }
        };
        for (auto &a : args) {
            auto id = a->GetReturnType().id();
            if (id == LogicalTypeId::SQLNULL) continue;
            if (id == LogicalTypeId::VARCHAR) { any_varchar = true; continue; }
            any_numeric = true;
            if (rank(id) > rank(widest_numeric)) widest_numeric = id;
        }
        if (any_varchar) {
            return_type = LogicalType::VARCHAR();
        } else if (any_numeric) {
            switch (widest_numeric) {
            case LogicalTypeId::DOUBLE:   return_type = LogicalType::DOUBLE(); break;
            case LogicalTypeId::FLOAT:    return_type = LogicalType::DOUBLE(); break;
            case LogicalTypeId::BIGINT:   return_type = LogicalType::BIGINT(); break;
            case LogicalTypeId::INTEGER:  return_type = LogicalType::INTEGER(); break;
            case LogicalTypeId::SMALLINT: return_type = LogicalType::INTEGER(); break;
            case LogicalTypeId::TINYINT:  return_type = LogicalType::INTEGER(); break;
            default:                      return_type = LogicalType::INTEGER(); break;
            }
        } else {
            return_type = LogicalType::SQLNULL();
        }
        for (auto &a : args) {
            if (a->GetReturnType().id() != return_type.id() &&
                a->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                a = std::make_unique<BoundCast>(std::move(a), return_type);
            }
        }
    }
    // Additional date functions.
    else if (name == "DATE_DIFF" || name == "DATEDIFF" || name == "TIMESTAMPDIFF" ||
             name == "AGE") {
        return_type = LogicalType::BIGINT();
    } else if (name == "DATE_ADD" || name == "DATEADD") {
        // Return type:
        // - TIMESTAMP if input is TIMESTAMP, or if the unit is sub-day
        //   (HOUR/MINUTE/SECOND/MILLISECOND/MICROSECOND) — the result
        //   no longer aligns to a DATE.
        // - DATE if input is DATE AND unit is DAY/WEEK (statically
        //   determinable from a literal first arg).
        // The executor independently re-checks the unit so a non-literal
        // part still produces correct values at runtime; the binder
        // sets the static upper-bound type so downstream consumers
        // (CAST, comparisons, storage) see the correct shape.
        bool input_is_date = false;
        if (args.size() >= 3 &&
            args[2]->GetReturnType().id() == LogicalTypeId::DATE) {
            input_is_date = true;
        }
        bool day_grain = false;
        if (input_is_date && !args.empty()) {
            // Peek if the first arg is a string literal we can read at
            // bind time. If so, decide DATE vs TIMESTAMP; if not, fall
            // back to TIMESTAMP to be safe.
            if (auto *bc = dynamic_cast<BoundConstant *>(args[0].get())) {
                if (bc->value.type().id() == LogicalTypeId::VARCHAR) {
                    auto part = StringUtil::Upper(bc->value.GetValue<std::string>());
                    day_grain = (part == "DAY" || part == "DAYS" ||
                                 part == "WEEK" || part == "WEEKS" ||
                                 part == "MONTH" || part == "MONTHS" ||
                                 part == "QUARTER" || part == "QUARTERS" ||
                                 part == "YEAR" || part == "YEARS");
                }
            }
        }
        if (input_is_date && day_grain) {
            return_type = LogicalType::DATE();
        } else {
            return_type = LogicalType::TIMESTAMP();
        }
    } else if (name == "STRFTIME" || name == "FORMAT_TIMESTAMP" ||
               name == "MONTHNAME" || name == "DAYNAME") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "LAST_DAY") {
        return_type = LogicalType::DATE();
    } else if (name == "MAKE_DATE") {
        return_type = LogicalType::DATE();
    }
    // Aggregate functions that work both as window and regular.
    else if (name == "STRING_AGG" || name == "LISTAGG" || name == "GROUP_CONCAT") {
        return_type = LogicalType::VARCHAR();
        is_agg = true;
    } else if (name == "STDDEV" || name == "STDDEV_SAMP" || name == "STDDEV_POP" ||
               name == "VARIANCE" || name == "VAR_SAMP" || name == "VAR_POP") {
        return_type = LogicalType::DOUBLE();
        is_agg = true;
    } else if (name == "BOOL_AND" || name == "BOOL_OR") {
        return_type = LogicalType::BOOLEAN();
        is_agg = true;
    } else if (name == "MEDIAN") {
        return_type = LogicalType::DOUBLE();
        is_agg = true;
    }

    // SQL:2003 FILTER (WHERE ...) — bind in the same row context as the
    // function arguments. Only valid on aggregate functions; reject on
    // scalars.
    //
    // Strategy: rewrite to a CASE-lift at bind time. FUNC(arg) FILTER (WHERE c)
    // becomes FUNC(CASE WHEN c THEN arg END). The CASE returns NULL when c is
    // false (or NULL — matches SQL standard), and SUM / COUNT / AVG / MIN /
    // MAX all already treat NULL as absent. For COUNT(*) which has no arg,
    // the rewrite is COUNT(CASE WHEN c THEN 1 END). This keeps every code
    // path that handles `col_idx == INVALID_INDEX` (the per-row expression
    // eval branch in ComputeAggregates) on the same hot path — no separate
    // filter plumbing needed in the aggregator.
    if (expr.filter) {
        if (!is_agg) {
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "FILTER clause is only valid on aggregate functions, not on '" + name + "'");
        }
        auto bound_filter = BindExpression(*expr.filter, context);
        if (args.empty()) {
            // COUNT(*) FILTER (WHERE c) -> COUNT(CASE WHEN c THEN 1 END).
            std::vector<BoundExprPtr> case_args;
            case_args.push_back(std::move(bound_filter));
            case_args.push_back(std::make_unique<BoundConstant>(Value::INTEGER(1)));
            args.push_back(std::make_unique<BoundFunction>(
                "CASE", std::move(case_args), LogicalType::INTEGER(), false));
        } else if (args.size() == 1) {
            // FUNC(arg) FILTER (WHERE c) -> FUNC(CASE WHEN c THEN arg END).
            auto arg_type = args[0]->GetReturnType();
            std::vector<BoundExprPtr> case_args;
            case_args.push_back(std::move(bound_filter));
            case_args.push_back(std::move(args[0]));
            args[0] = std::make_unique<BoundFunction>(
                "CASE", std::move(case_args), arg_type, false);
        } else {
            // Multi-arg aggregates (STRING_AGG, etc) with FILTER need each
            // arg wrapped in the same CASE. The filter must be cloneable
            // for that; current BoundExpression has no clone. Reject
            // explicitly so this surfaces at bind time rather than being
            // silently wrong.
            throw BinderException(ErrorCode::TYPE_MISMATCH,
                "FILTER on multi-argument aggregate '" + name + "' is not yet supported");
        }
    }

    return std::make_unique<BoundFunction>(name, std::move(args), return_type,
                                            is_agg, expr.is_distinct);
}

BoundExprPtr Binder::BindCast(const CastExpression &expr, BindContext &context) {
    auto child = BindExpression(*expr.child, context);
    auto target = ResolveTypeName(expr.target_type);
    return std::make_unique<BoundCast>(std::move(child), target, expr.is_try);
}

BoundExprPtr Binder::BindWindow(const WindowExpression &expr, BindContext &context) {
    auto name = StringUtil::Upper(expr.function_name);

    std::vector<BoundExprPtr> args;
    for (auto &arg : expr.arguments) {
        args.push_back(BindExpression(*arg, context));
    }

    // Determine return type.
    LogicalType return_type = LogicalType::BIGINT();
    if (name == "ROW_NUMBER" || name == "RANK" || name == "DENSE_RANK" || name == "NTILE") {
        return_type = LogicalType::BIGINT();
    } else if (name == "LEAD" || name == "LAG" || name == "FIRST_VALUE" ||
               name == "LAST_VALUE" || name == "NTH_VALUE") {
        return_type = args.empty() ? LogicalType::SQLNULL() : args[0]->GetReturnType();
    } else if (name == "SUM") {
        return_type = args.empty() ? LogicalType::BIGINT() : args[0]->GetReturnType();
        if (return_type.id() == LogicalTypeId::INTEGER) return_type = LogicalType::BIGINT();
    } else if (name == "COUNT") {
        return_type = LogicalType::BIGINT();
    } else if (name == "AVG") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "MIN" || name == "MAX") {
        return_type = args.empty() ? LogicalType::SQLNULL() : args[0]->GetReturnType();
    }

    auto window = std::make_unique<BoundWindowExpression>(name, std::move(args), return_type);

    for (auto &p : expr.partition_by) {
        window->partition_by.push_back(BindExpression(*p, context));
    }
    for (auto &o : expr.order_by) {
        BoundWindowOrder bo;
        bo.expression = BindExpression(*o.expression, context);
        bo.ascending = o.ascending;
        window->order_by.push_back(std::move(bo));
    }
    // SQL:2003 FILTER on window aggregates — same per-row semantics as
    // BindFunction's filter (NULL filter result acts as false).
    if (expr.filter) {
        window->filter = BindExpression(*expr.filter, context);
    }

    return window;
}

BoundExprPtr Binder::BindSubquery(const SubqueryExpression &expr, BindContext &context) {
    BoundSubqueryExpression::Type subtype;
    LogicalType return_type = LogicalType::BOOLEAN();

    switch (expr.subquery_type) {
    case SubqueryType::EXISTS:
        subtype = BoundSubqueryExpression::Type::EXISTS;
        break;
    case SubqueryType::NOT_EXISTS:
        subtype = BoundSubqueryExpression::Type::NOT_EXISTS;
        break;
    case SubqueryType::SCALAR: {
        subtype = BoundSubqueryExpression::Type::SCALAR;
        // Bind the subquery to discover its first projection's type.
        // Without this the return_type stays SQLNULL, which makes
        // every `expr = (SELECT ...)` short-circuit to NULL via the
        // SQLNULL-operand branch in ExecuteComparison. The inner
        // binder is re-created so the subquery doesn't share our
        // outer BindContext (correlated subqueries deferred).
        if (expr.subquery) {
            Binder inner_binder(catalog_);
            try {
                auto inner = inner_binder.Bind(*expr.subquery);
                if (inner) {
                    auto &bs = static_cast<BoundSelectStatement &>(*inner);
                    if (!bs.result_types.empty()) {
                        return_type = bs.result_types[0];
                    }
                }
            } catch (...) {
                // Fall back to SQLNULL — runtime path still works for
                // EXPLAIN / catalog-dependent inner SELECT.
                return_type = LogicalType::SQLNULL();
            }
        } else {
            return_type = LogicalType::SQLNULL();
        }
        break;
    }
    case SubqueryType::IN_SUBQUERY:
        subtype = BoundSubqueryExpression::Type::IN_SUBQUERY;
        break;
    }

    auto result = std::make_unique<BoundSubqueryExpression>(subtype, return_type);

    // Store the parsed query so we can execute it at runtime.
    // Use shared_ptr<void> with a custom no-op deleter since we don't own the query.
    // The parsed statement tree is owned by the Parser output.
    // Actually, we need to clone or keep the pointer alive. Since ParsedStatement
    // is owned by the statement vector in Connection::Query, we store a raw pointer
    // wrapped in shared_ptr with no-op deleter.
    result->parsed_query = std::shared_ptr<void>(
        const_cast<SelectStatement *>(expr.subquery.get()),
        [](void *) {}); // non-owning

    // Bind child expression for IN subquery.
    if (expr.child) {
        result->child = BindExpression(*expr.child, context);
    }

    return result;
}

BoundExprPtr Binder::BindStar(const StarExpression &expr, BindContext &context,
                               std::vector<BoundExprPtr> &expanded) {
    if (!expr.table_name.empty()) {
        auto it = context.tables.find(expr.table_name);
        if (it == context.tables.end()) {
            throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                                   "Table '" + expr.table_name + "' not found");
        }
        auto *entry = it->second;
        idx_t offset = context.GetTableOffset(expr.table_name);
        for (idx_t i = 0; i < entry->ColumnCount(); i++) {
            auto &col = entry->GetColumns()[i];
            auto ref = std::make_unique<BoundColumnRef>(col.name, offset + i, col.type);
            ref->alias = col.name;
            expanded.push_back(std::move(ref));
        }
    } else {
        // Expand in table insertion order.
        for (auto &[alias, offset] : context.table_order) {
            auto *entry = context.tables.at(alias);
            for (idx_t i = 0; i < entry->ColumnCount(); i++) {
                auto &col = entry->GetColumns()[i];
                auto ref = std::make_unique<BoundColumnRef>(col.name, offset + i, col.type);
                ref->alias = col.name;
                expanded.push_back(std::move(ref));
            }
        }
    }
    return nullptr;
}

// ============================================================================
// Type resolution
// ============================================================================

LogicalType Binder::ResolveTypeName(const std::string &name) {
    auto upper = StringUtil::Upper(name);

    // Strip parameters: DECIMAL(10,2) -> DECIMAL
    auto base = upper.substr(0, upper.find('('));

    if (base == "INTEGER" || base == "INT" || base == "INT4") return LogicalType::INTEGER();
    if (base == "BIGINT" || base == "INT8") return LogicalType::BIGINT();
    if (base == "SMALLINT" || base == "INT2") return LogicalType::SMALLINT();
    if (base == "TINYINT" || base == "INT1") return LogicalType::TINYINT();
    if (base == "HUGEINT") return LogicalType::HUGEINT();
    if (base == "FLOAT" || base == "REAL" || base == "FLOAT4") return LogicalType::FLOAT();
    if (base == "DOUBLE" || base == "FLOAT8") return LogicalType::DOUBLE();
    if (base == "BOOLEAN" || base == "BOOL") return LogicalType::BOOLEAN();
    if (base == "VARCHAR" || base == "TEXT" || base == "STRING" || base == "CHAR") {
        // Preserve VARCHAR(n) length so PhysicalInsert can reject over-long
        // strings. TEXT / STRING are unbounded in standard SQL, so only
        // attach the length when the user actually wrote it.
        auto paren = upper.find('(');
        if (paren != std::string::npos) {
            auto inner = upper.substr(paren + 1, upper.size() - paren - 2);
            idx_t n = static_cast<idx_t>(std::stoull(inner));
            return LogicalType::VARCHAR_N(n);
        }
        return LogicalType::VARCHAR();
    }
    if (base == "BLOB" || base == "BYTEA") return LogicalType::BLOB();
    if (base == "DATE") return LogicalType::DATE();
    if (base == "TIME") return LogicalType::TIME();
    if (base == "TIMESTAMP" || base == "TIMESTAMPTZ" ||
        base == "TIMESTAMP_S" || base == "TIMESTAMP_MS" ||
        base == "TIMESTAMP_NS" || base == "DATETIME")
        return LogicalType::TIMESTAMP();
    if (base == "INTERVAL") return LogicalType::INTERVAL();
    if (base == "UUID") return LogicalType::UUID();

    if (base == "DECIMAL" || base == "NUMERIC") {
        // SlothDB has no native DECIMAL execution path: INSERT into
        // DECIMAL columns silently stored zeros, SUM/AVG returned 0,
        // and CAST AS DECIMAL produced 0. Until a real fixed-point
        // codepath lands, alias DECIMAL/NUMERIC to DOUBLE so the
        // queries work with approximate semantics instead of silent
        // wrong results. Width/scale parameters are parsed and
        // discarded (matches MySQL's "approximate DECIMAL" pragma).
        return LogicalType::DOUBLE();
    }

    throw BinderException(ErrorCode::TYPE_MISMATCH, "Unknown type: " + name);
}

LogicalType Binder::ResolveArithmeticType(const LogicalType &left, const LogicalType &right) {
    // Simple numeric promotion rules.
    if (left.id() == LogicalTypeId::DOUBLE || right.id() == LogicalTypeId::DOUBLE)
        return LogicalType::DOUBLE();
    if (left.id() == LogicalTypeId::FLOAT || right.id() == LogicalTypeId::FLOAT)
        return LogicalType::DOUBLE();
    if (left.id() == LogicalTypeId::BIGINT || right.id() == LogicalTypeId::BIGINT)
        return LogicalType::BIGINT();
    if (left.id() == LogicalTypeId::INTEGER || right.id() == LogicalTypeId::INTEGER)
        return LogicalType::INTEGER();
    if (left.id() == LogicalTypeId::SMALLINT || right.id() == LogicalTypeId::SMALLINT)
        return LogicalType::INTEGER();
    return LogicalType::INTEGER();
}

// Recursively check whether a bound expression contains any aggregate
// function. Used by WHERE / UPDATE-SET / DELETE-WHERE validation to
// raise a clean BinderException instead of letting an aggregate slip
// into row-context evaluation. Subqueries are NOT recursed into —
// aggregates inside a correlated subquery are legal.
static bool ContainsAggregate(const BoundExpression &expr) {
    switch (expr.GetExpressionType()) {
    case BoundExpressionType::FUNCTION: {
        auto &fn = static_cast<const BoundFunction &>(expr);
        if (fn.is_aggregate) return true;
        for (auto &a : fn.arguments) {
            if (a && ContainsAggregate(*a)) return true;
        }
        if (fn.filter && ContainsAggregate(*fn.filter)) return true;
        return false;
    }
    case BoundExpressionType::COMPARISON: {
        auto &c = static_cast<const BoundComparison &>(expr);
        return (c.left && ContainsAggregate(*c.left)) ||
               (c.right && ContainsAggregate(*c.right));
    }
    case BoundExpressionType::CONJUNCTION: {
        auto &c = static_cast<const BoundConjunction &>(expr);
        return (c.left && ContainsAggregate(*c.left)) ||
               (c.right && ContainsAggregate(*c.right));
    }
    case BoundExpressionType::NEGATION: {
        auto &n = static_cast<const BoundNegation &>(expr);
        return n.child && ContainsAggregate(*n.child);
    }
    case BoundExpressionType::IS_NULL: {
        auto &i = static_cast<const BoundIsNull &>(expr);
        return i.child && ContainsAggregate(*i.child);
    }
    case BoundExpressionType::IS_BOOL: {
        auto &i = static_cast<const BoundIsBool &>(expr);
        return i.child && ContainsAggregate(*i.child);
    }
    case BoundExpressionType::ARITHMETIC: {
        auto &a = static_cast<const BoundArithmetic &>(expr);
        return (a.left && ContainsAggregate(*a.left)) ||
               (a.right && ContainsAggregate(*a.right));
    }
    case BoundExpressionType::UNARY_MINUS: {
        auto &u = static_cast<const BoundUnaryMinus &>(expr);
        return u.child && ContainsAggregate(*u.child);
    }
    case BoundExpressionType::CAST: {
        auto &c = static_cast<const BoundCast &>(expr);
        return c.child && ContainsAggregate(*c.child);
    }
    case BoundExpressionType::SUBQUERY:
        // Aggregates inside a correlated subquery are legal at SQL
        // standard level; do not recurse.
        return false;
    default:
        return false;
    }
}

bool Binder::IsAggregateFunction(const std::string &name) const {
    return name == "COUNT" || name == "SUM" || name == "AVG" ||
           name == "MIN" || name == "MAX" ||
           name == "STRING_AGG" || name == "LISTAGG" || name == "GROUP_CONCAT" ||
           name == "STDDEV" || name == "STDDEV_SAMP" || name == "STDDEV_POP" ||
           name == "VARIANCE" || name == "VAR_SAMP" || name == "VAR_POP" ||
           name == "MEDIAN" || name == "BOOL_AND" || name == "BOOL_OR";
}

} // namespace slothdb

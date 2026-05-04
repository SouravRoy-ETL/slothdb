#include "slothdb/binder/binder.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include "slothdb/parser/expression/parsed_expression.hpp"

namespace slothdb {

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

            // Detect JOIN.
            if (stmt.from_table->right && !stmt.from_table->join_type.empty()) {
                auto join_info = std::make_unique<BoundSelectStatement::JoinInfo>();
                auto right_alias = stmt.from_table->right->alias.empty()
                    ? stmt.from_table->right->table_name : stmt.from_table->right->alias;
                auto right_it = context.tables.find(right_alias);
                if (right_it != context.tables.end()) {
                    join_info->right_table = right_it->second;
                    join_info->right_alias = right_alias;
                }
                join_info->join_type = stmt.from_table->join_type;
                join_info->left_col_count = result->table->ColumnCount();
                join_info->right_col_count = join_info->right_table
                    ? join_info->right_table->ColumnCount() : 0;

                // Bind ON condition.
                if (stmt.from_table->on_condition) {
                    join_info->condition = BindExpression(
                        *stmt.from_table->on_condition, context);
                }
                result->join = std::move(join_info);
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

    // Bind WHERE.
    if (stmt.where_clause) {
        result->where_clause = BindExpression(*stmt.where_clause, context);
    }

    // Bind GROUP BY. Resolve positional ordinals (`GROUP BY 1`) and
    // select-list aliases (e.g. `extract(minute FROM EventTime) AS m ...
    // GROUP BY m`) to their underlying SELECT-list expression. ClickBench
    // Q13 needs the ordinal path; Q19 needs the alias path.
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
                if (ord >= 1 && ord <= static_cast<int64_t>(stmt.select_list.size())) {
                    idx_t sl_idx = static_cast<idx_t>(ord - 1);
                    if (stmt.select_list[sl_idx]->GetExpressionType()
                            != ExpressionType::STAR) {
                        result->group_by.push_back(
                            BindExpression(*stmt.select_list[sl_idx], context));
                        resolved = true;
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

    // Bind HAVING.
    if (stmt.having_clause) {
        result->having_clause = BindExpression(*stmt.having_clause, context);
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
        // path above. Required for ClickBench Q13 ("ORDER BY 2 DESC"
        // sorts by COUNT(*), not by the constant 2). Out-of-range
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
                    if (ord >= 1 &&
                        ord <= static_cast<int64_t>(stmt.select_list.size())) {
                        idx_t sl_idx = static_cast<idx_t>(ord - 1);
                        if (stmt.select_list[sl_idx]->GetExpressionType()
                                != ExpressionType::STAR) {
                            bound_item.expression = BindExpression(
                                *stmt.select_list[sl_idx], context);
                            resolved = true;
                        }
                    } else {
                        throw BinderException(
                            ErrorCode::OUT_OF_RANGE,
                            "ORDER BY position " + std::to_string(ord) +
                            " is out of range (select list has " +
                            std::to_string(stmt.select_list.size()) +
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
        result->order_by.push_back(std::move(bound_item));
    }

    // Bind LIMIT/OFFSET.
    if (stmt.limit) {
        auto bound = BindExpression(*stmt.limit, context);
        if (bound->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &val = static_cast<BoundConstant &>(*bound).value;
            if (val.type().id() == LogicalTypeId::INTEGER) {
                result->limit_count = val.GetValue<int32_t>();
            } else {
                result->limit_count = val.GetValue<int64_t>();
            }
        }
    }
    if (stmt.offset) {
        auto bound = BindExpression(*stmt.offset, context);
        if (bound->GetExpressionType() == BoundExpressionType::CONSTANT) {
            auto &val = static_cast<BoundConstant &>(*bound).value;
            if (val.type().id() == LogicalTypeId::INTEGER) {
                result->offset_count = val.GetValue<int32_t>();
            } else {
                result->offset_count = val.GetValue<int64_t>();
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
        result->columns.emplace_back(col.name, type);
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
    for (auto &row : stmt.values) {
        std::vector<BoundExprPtr> bound_row;
        for (auto &expr : row) {
            bound_row.push_back(BindExpression(*expr, context));
        }
        result->values.push_back(std::move(bound_row));
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

    for (auto &assign : stmt.assignments) {
        auto idx = entry->GetColumnIndex(assign.column_name);
        if (idx == INVALID_INDEX) {
            throw BinderException(ErrorCode::COLUMN_NOT_FOUND,
                                   "Column '" + assign.column_name + "' not found");
        }
        BoundUpdateAssignment ba;
        ba.column_index = idx;
        ba.value = BindExpression(*assign.value, context);
        result->assignments.push_back(std::move(ba));
    }

    if (stmt.where_clause) {
        result->where_clause = BindExpression(*stmt.where_clause, context);
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

    // Recursively bind the right side of joins.
    if (ref.right) {
        auto *right_entry = catalog_.GetTable(ref.right->table_name);
        if (!right_entry) {
            throw BinderException(ErrorCode::TABLE_NOT_FOUND,
                                   "Table '" + ref.right->table_name + "' not found");
        }
        auto right_alias = ref.right->alias.empty()
                             ? ref.right->table_name : ref.right->alias;
        context.AddTable(right_alias, right_entry);
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
        int64_t val = std::stoll(expr.value);
        if (val >= INT32_MIN && val <= INT32_MAX) {
            return std::make_unique<BoundConstant>(Value::INTEGER(static_cast<int32_t>(val)));
        }
        return std::make_unique<BoundConstant>(Value::BIGINT(val));
    }
    case TokenType::FLOAT_LITERAL:
        return std::make_unique<BoundConstant>(Value::DOUBLE(std::stod(expr.value)));
    case TokenType::STRING_LITERAL:
        return std::make_unique<BoundConstant>(Value::VARCHAR(expr.value));
    case TokenType::KW_TRUE:
        return std::make_unique<BoundConstant>(Value::BOOLEAN(true));
    case TokenType::KW_FALSE:
        return std::make_unique<BoundConstant>(Value::BOOLEAN(false));
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
    if (left->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
        promote_literal(*left, right.get());
    } else if (right->GetExpressionType() == BoundExpressionType::COLUMN_REF) {
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

BoundExprPtr Binder::BindArithmetic(const ArithmeticExpression &expr, BindContext &context) {
    auto left = BindExpression(*expr.left, context);
    auto right = BindExpression(*expr.right, context);
    // || is string concatenation.
    if (expr.op == "||") {
        return std::make_unique<BoundArithmetic>(expr.op, std::move(left), std::move(right),
                                                  LogicalType::VARCHAR());
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
    bool is_agg = IsAggregateFunction(name);

    std::vector<BoundExprPtr> args;
    for (auto &arg : expr.arguments) {
        args.push_back(BindExpression(*arg, context));
    }

    // Determine return type.
    LogicalType return_type = LogicalType::INTEGER(); // default
    if (name == "COUNT") {
        return_type = LogicalType::BIGINT();
    } else if (name == "SUM") {
        return_type = args.empty() ? LogicalType::BIGINT() : args[0]->GetReturnType();
        // SUM of integers -> BIGINT.
        if (return_type.id() == LogicalTypeId::INTEGER ||
            return_type.id() == LogicalTypeId::SMALLINT ||
            return_type.id() == LogicalTypeId::TINYINT) {
            return_type = LogicalType::BIGINT();
        }
    } else if (name == "AVG") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "MIN" || name == "MAX") {
        return_type = args.empty() ? LogicalType::INTEGER() : args[0]->GetReturnType();
    }
    // Scalar function return types.
    else if (name == "LENGTH" || name == "STRLEN") {
        return_type = LogicalType::INTEGER();
    } else if (name == "UPPER" || name == "LOWER" || name == "TRIM" ||
               name == "LTRIM" || name == "RTRIM" || name == "REPLACE" ||
               name == "SUBSTRING" || name == "SUBSTR" || name == "CONCAT") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "ABS") {
        return_type = args.empty() ? LogicalType::INTEGER() : args[0]->GetReturnType();
    } else if (name == "CEIL" || name == "CEILING" || name == "FLOOR" ||
               name == "ROUND" || name == "SQRT" || name == "POWER" || name == "MOD") {
        return_type = LogicalType::DOUBLE();
    } else if (name == "COALESCE" || name == "IFNULL" || name == "NVL") {
        // Return type comes from the first non-NULL-typed argument.
        // (NULL literals carry SQLNULL type — skip them so a query like
        // COALESCE(NULL, NULL, 3) still resolves to INTEGER.)
        return_type = LogicalType::SQLNULL();
        for (auto &a : args) {
            if (a->GetReturnType().id() != LogicalTypeId::SQLNULL) {
                return_type = a->GetReturnType();
                break;
            }
        }
    } else if (name == "NULLIF") {
        return_type = args.empty() ? LogicalType::SQLNULL() : args[0]->GetReturnType();
    } else if (name == "CASE" || name == "IF" || name == "IIF") {
        // CASE(when, then, [when, then, ...] [else]) — return = type of
        // first THEN. IF/IIF(cond, then, else) lay out args identically.
        return_type = args.size() >= 2 ? args[1]->GetReturnType() : LogicalType::SQLNULL();
    } else if (name == "IN" || name == "BETWEEN") {
        return_type = LogicalType::BOOLEAN();
    } else if (name == "NOW" || name == "CURRENT_TIMESTAMP" || name == "TO_TIMESTAMP" ||
               name == "MAKE_TIMESTAMP" || name == "DATE_TRUNC") {
        return_type = LogicalType::BIGINT(); // microseconds since epoch
    } else if (name == "CURRENT_DATE") {
        return_type = LogicalType::INTEGER(); // YYYYMMDD
    } else if (name == "EXTRACT" || name == "DATE_PART") {
        return_type = LogicalType::BIGINT();
    } else if (name == "EPOCH_MS") {
        return_type = LogicalType::DOUBLE();
    }
    // Additional string functions.
    else if (name == "POSITION" || name == "STRPOS") {
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
             name == "ASIN" || name == "ACOS" || name == "ATAN") {
        return_type = LogicalType::DOUBLE();
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
        return_type = args.empty() ? LogicalType::INTEGER() : args[0]->GetReturnType();
    }
    // Additional date functions.
    else if (name == "DATE_DIFF" || name == "DATEDIFF" || name == "DATE_ADD" ||
             name == "DATEADD") {
        return_type = LogicalType::BIGINT();
    } else if (name == "STRFTIME" || name == "FORMAT_TIMESTAMP" ||
               name == "MONTHNAME" || name == "DAYNAME") {
        return_type = LogicalType::VARCHAR();
    } else if (name == "LAST_DAY") {
        return_type = LogicalType::BIGINT(); // microseconds since epoch
    } else if (name == "MAKE_DATE") {
        return_type = LogicalType::INTEGER(); // YYYYMMDD
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
    case SubqueryType::SCALAR:
        subtype = BoundSubqueryExpression::Type::SCALAR;
        // Try to determine return type from subquery.
        return_type = LogicalType::SQLNULL();
        break;
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
    if (base == "TIMESTAMP") return LogicalType::TIMESTAMP();
    if (base == "INTERVAL") return LogicalType::INTERVAL();
    if (base == "UUID") return LogicalType::UUID();

    if (base == "DECIMAL" || base == "NUMERIC") {
        // Parse DECIMAL(width, scale).
        auto paren = upper.find('(');
        if (paren != std::string::npos) {
            auto inner = upper.substr(paren + 1, upper.size() - paren - 2);
            auto comma = inner.find(',');
            uint8_t width = static_cast<uint8_t>(std::stoi(inner.substr(0, comma)));
            uint8_t scale = comma != std::string::npos
                              ? static_cast<uint8_t>(std::stoi(inner.substr(comma + 1)))
                              : 0;
            return LogicalType::DECIMAL(width, scale);
        }
        return LogicalType::DECIMAL(18, 3);
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

bool Binder::IsAggregateFunction(const std::string &name) const {
    return name == "COUNT" || name == "SUM" || name == "AVG" ||
           name == "MIN" || name == "MAX" ||
           name == "STRING_AGG" || name == "LISTAGG" || name == "GROUP_CONCAT" ||
           name == "STDDEV" || name == "STDDEV_SAMP" || name == "STDDEV_POP" ||
           name == "VARIANCE" || name == "VAR_SAMP" || name == "VAR_POP" ||
           name == "MEDIAN" || name == "BOOL_AND" || name == "BOOL_OR";
}

} // namespace slothdb

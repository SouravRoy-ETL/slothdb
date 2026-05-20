#pragma once

#include "slothdb/binder/bound_statement.hpp"
#include "slothdb/parser/statement/parsed_statement.hpp"
#include "slothdb/catalog/catalog.hpp"
#include <string>
#include <unordered_map>

namespace slothdb {

// Bind context: tracks which columns are available during binding.
struct BindContext {
    // table_name -> TableCatalogEntry*
    std::unordered_map<std::string, TableCatalogEntry *> tables;
    // column_name -> (table_name, column_index_in_combined_output)
    std::unordered_map<std::string, std::pair<std::string, idx_t>> columns;
    // Table insertion order + cumulative column offset for JOINs.
    std::vector<std::pair<std::string, idx_t>> table_order; // (alias, column_offset)

    // SELECT-list alias map (UPPER(name) -> parsed expression). Populated
    // only while binding HAVING so that `HAVING c > N` can resolve `c`
    // against `SELECT ... COUNT(*) AS c`. Empty otherwise.
    std::unordered_map<std::string, const ParsedExpression *> select_list_aliases;

    void AddTable(const std::string &alias, TableCatalogEntry *entry);
    std::pair<std::string, idx_t> ResolveColumn(const std::string &col_name,
                                                 const std::string &table_name = "") const;
    LogicalType GetColumnType(const std::string &col_name,
                               const std::string &table_name = "") const;
    idx_t GetTableOffset(const std::string &alias) const;
};

// The Binder: resolves names and types in parsed statements against the catalog.
class Binder {
public:
    explicit Binder(Catalog &catalog);

    BoundStmtPtr Bind(const ParsedStatement &stmt);

private:
    BoundStmtPtr BindSelect(const SelectStatement &stmt);
    BoundStmtPtr BindCreateTable(const CreateTableStatement &stmt);
    BoundStmtPtr BindDropTable(const DropTableStatement &stmt);
    BoundStmtPtr BindInsert(const InsertStatement &stmt);
    BoundStmtPtr BindUpdate(const UpdateStatement &stmt);
    BoundStmtPtr BindDelete(const DeleteStatement &stmt);

    // Bind FROM clause into the context.
    void BindTableRef(const TableRef &ref, BindContext &context);

    // Bind expressions.
    BoundExprPtr BindExpression(const ParsedExpression &expr, BindContext &context);
    BoundExprPtr BindColumnRef(const ColumnRefExpression &expr, BindContext &context);
    BoundExprPtr BindConstant(const ConstantExpression &expr);
    BoundExprPtr BindComparison(const ComparisonExpression &expr, BindContext &context);
    BoundExprPtr BindConjunction(const ConjunctionExpression &expr, BindContext &context);
    BoundExprPtr BindNegation(const NegationExpression &expr, BindContext &context);
    BoundExprPtr BindIsNull(const IsNullExpression &expr, BindContext &context);
    BoundExprPtr BindIsBool(const IsBoolExpression &expr, BindContext &context);
    BoundExprPtr BindArithmetic(const ArithmeticExpression &expr, BindContext &context);
    BoundExprPtr BindUnaryMinus(const UnaryMinusExpression &expr, BindContext &context);
    BoundExprPtr BindFunction(const FunctionExpression &expr, BindContext &context);
    BoundExprPtr BindCast(const CastExpression &expr, BindContext &context);
    BoundExprPtr BindWindow(const WindowExpression &expr, BindContext &context);
    BoundExprPtr BindSubquery(const SubqueryExpression &expr, BindContext &context);
    BoundExprPtr BindStar(const StarExpression &expr, BindContext &context,
                          std::vector<BoundExprPtr> &expanded);

public:
    // Type resolution helpers (public for ALTER TABLE).
    LogicalType ResolveTypeName(const std::string &name);
private:
    LogicalType ResolveArithmeticType(const LogicalType &left, const LogicalType &right);
    bool IsAggregateFunction(const std::string &name) const;

    Catalog &catalog_;
};

} // namespace slothdb

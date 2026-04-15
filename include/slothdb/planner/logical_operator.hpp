#pragma once

#include "slothdb/binder/bound_expression.hpp"
#include "slothdb/binder/bound_statement.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/catalog/table_catalog_entry.hpp"
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

enum class LogicalOperatorType : uint8_t {
    GET,
    FILTER,
    PROJECTION,
    ORDER_BY,
    LIMIT,
    INSERT,
    CREATE_TABLE,
    DROP_TABLE,
    UPDATE,
    DELETE_STMT,
    AGGREGATE,
    JOIN,
    DISTINCT,
    WINDOW,
    DUMMY_SCAN    // For queries without FROM (SELECT 1)
};

class LogicalOperator {
public:
    LogicalOperator(LogicalOperatorType type, std::vector<LogicalType> types)
        : type_(type), types_(std::move(types)) {}
    virtual ~LogicalOperator() = default;

    LogicalOperatorType GetOperatorType() const { return type_; }
    const std::vector<LogicalType> &GetTypes() const { return types_; }

    std::vector<std::unique_ptr<LogicalOperator>> children;

private:
    LogicalOperatorType type_;
    std::vector<LogicalType> types_;
};

using LogicalOpPtr = std::unique_ptr<LogicalOperator>;

// Table scan.
class LogicalGet : public LogicalOperator {
public:
    LogicalGet(TableCatalogEntry *table)
        : LogicalOperator(LogicalOperatorType::GET, table->GetTypes()),
          table(table) {}

    TableCatalogEntry *table;
};

// Filter (WHERE clause).
class LogicalFilter : public LogicalOperator {
public:
    LogicalFilter(BoundExprPtr condition, std::vector<LogicalType> types)
        : LogicalOperator(LogicalOperatorType::FILTER, std::move(types)),
          condition(std::move(condition)) {}

    BoundExprPtr condition;
};

// Projection (SELECT list).
class LogicalProjection : public LogicalOperator {
public:
    LogicalProjection(std::vector<BoundExprPtr> expressions,
                      std::vector<LogicalType> types)
        : LogicalOperator(LogicalOperatorType::PROJECTION, std::move(types)),
          expressions(std::move(expressions)) {}

    std::vector<BoundExprPtr> expressions;
};

// ORDER BY.
class LogicalOrderBy : public LogicalOperator {
public:
    LogicalOrderBy(std::vector<BoundOrderBy> orders, std::vector<LogicalType> types)
        : LogicalOperator(LogicalOperatorType::ORDER_BY, std::move(types)),
          orders(std::move(orders)) {}

    std::vector<BoundOrderBy> orders;
};

// LIMIT [OFFSET].
class LogicalLimit : public LogicalOperator {
public:
    LogicalLimit(int64_t limit_count, int64_t offset_count,
                 std::vector<LogicalType> types)
        : LogicalOperator(LogicalOperatorType::LIMIT, std::move(types)),
          limit_count(limit_count), offset_count(offset_count) {}

    int64_t limit_count;
    int64_t offset_count;
};

// INSERT.
class LogicalInsert : public LogicalOperator {
public:
    LogicalInsert(TableCatalogEntry *table,
                  std::vector<std::vector<BoundExprPtr>> values)
        : LogicalOperator(LogicalOperatorType::INSERT, {}),
          table(table), values(std::move(values)) {}

    TableCatalogEntry *table;
    std::vector<std::vector<BoundExprPtr>> values;
};

// CREATE TABLE.
class LogicalCreateTable : public LogicalOperator {
public:
    LogicalCreateTable(const std::string &name,
                       std::vector<ColumnDefinition> columns, bool if_not_exists)
        : LogicalOperator(LogicalOperatorType::CREATE_TABLE, {}),
          table_name(name), columns(std::move(columns)),
          if_not_exists(if_not_exists) {}

    std::string table_name;
    std::vector<ColumnDefinition> columns;
    bool if_not_exists;
};

// DROP TABLE.
class LogicalDropTable : public LogicalOperator {
public:
    LogicalDropTable(const std::string &name, bool if_exists)
        : LogicalOperator(LogicalOperatorType::DROP_TABLE, {}),
          table_name(name), if_exists(if_exists) {}

    std::string table_name;
    bool if_exists;
};

// Aggregate (GROUP BY + aggregate functions).
class LogicalAggregate : public LogicalOperator {
public:
    LogicalAggregate(std::vector<BoundExprPtr> groups,
                     std::vector<BoundExprPtr> aggregates,
                     std::vector<LogicalType> result_types)
        : LogicalOperator(LogicalOperatorType::AGGREGATE, std::move(result_types)),
          groups(std::move(groups)), aggregates(std::move(aggregates)) {}

    std::vector<BoundExprPtr> groups;      // GROUP BY expressions
    std::vector<BoundExprPtr> aggregates;  // aggregate function calls
};

// Join.
enum class JoinType : uint8_t {
    INNER,
    LEFT,
    RIGHT,
    FULL,
    CROSS
};

class LogicalJoin : public LogicalOperator {
public:
    LogicalJoin(JoinType join_type, BoundExprPtr condition,
                std::vector<LogicalType> result_types)
        : LogicalOperator(LogicalOperatorType::JOIN, std::move(result_types)),
          join_type(join_type), condition(std::move(condition)) {}

    JoinType join_type;
    BoundExprPtr condition;
};

// UPDATE.
class LogicalUpdate : public LogicalOperator {
public:
    LogicalUpdate(TableCatalogEntry *table,
                  std::vector<BoundUpdateAssignment> assignments,
                  BoundExprPtr where_clause)
        : LogicalOperator(LogicalOperatorType::UPDATE, {}),
          table(table), assignments(std::move(assignments)),
          where_clause(std::move(where_clause)) {}

    TableCatalogEntry *table;
    std::vector<BoundUpdateAssignment> assignments;
    BoundExprPtr where_clause;
};

// DELETE.
class LogicalDeleteOp : public LogicalOperator {
public:
    LogicalDeleteOp(TableCatalogEntry *table, BoundExprPtr where_clause)
        : LogicalOperator(LogicalOperatorType::DELETE_STMT, {}),
          table(table), where_clause(std::move(where_clause)) {}

    TableCatalogEntry *table;
    BoundExprPtr where_clause;
};

// Window function computation.
class LogicalWindow : public LogicalOperator {
public:
    LogicalWindow(std::vector<BoundExprPtr> select_list,
                  BoundExprPtr qualify, std::vector<LogicalType> types)
        : LogicalOperator(LogicalOperatorType::WINDOW, std::move(types)),
          select_list(std::move(select_list)), qualify(std::move(qualify)) {}

    std::vector<BoundExprPtr> select_list;
    BoundExprPtr qualify;
};

// DISTINCT.
class LogicalDistinct : public LogicalOperator {
public:
    explicit LogicalDistinct(std::vector<LogicalType> types)
        : LogicalOperator(LogicalOperatorType::DISTINCT, std::move(types)) {}
};

// Dummy scan for constant queries (SELECT 1).
class LogicalDummyScan : public LogicalOperator {
public:
    LogicalDummyScan() : LogicalOperator(LogicalOperatorType::DUMMY_SCAN, {}) {}
};

} // namespace slothdb

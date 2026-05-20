#pragma once

#include "slothdb/binder/bound_expression.hpp"
#include "slothdb/catalog/table_catalog_entry.hpp"
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

enum class BoundStatementType : uint8_t {
    SELECT,
    CREATE_TABLE,
    DROP_TABLE,
    INSERT,
    UPDATE,
    DELETE_STMT
};

class BoundStatement {
public:
    explicit BoundStatement(BoundStatementType type) : type_(type) {}
    virtual ~BoundStatement() = default;
    BoundStatementType GetType() const { return type_; }

private:
    BoundStatementType type_;
};

using BoundStmtPtr = std::unique_ptr<BoundStatement>;

struct BoundOrderBy {
    BoundExprPtr expression;
    bool ascending = true;
    // SQL standard: NULL position defaults to NULLS LAST for ASC and
    // NULLS FIRST for DESC (Postgres/DuckDB convention). User can
    // override with explicit NULLS FIRST/LAST. The binder sets this
    // from the parser's OrderByItem, applying the direction default
    // when the user didn't say.
    bool nulls_first = false;
};

class BoundSelectStatement : public BoundStatement {
public:
    BoundSelectStatement() : BoundStatement(BoundStatementType::SELECT) {}

    bool is_distinct = false;

    // The table being scanned (nullptr for constant queries like SELECT 1).
    TableCatalogEntry *table = nullptr;
    std::string table_alias;

    // For JOINs: additional tables. Each JoinInfo is one chain link;
    // the planner builds a left-deep cascade by folding the vector
    // into nested LogicalJoin nodes. SQL-92 comma-separated FROM
    // (`FROM a, b, c`) produces multiple CROSS joins via this vector.
    struct JoinInfo {
        TableCatalogEntry *right_table = nullptr;
        std::string right_alias;
        std::string join_type; // "INNER", "LEFT", "RIGHT", "FULL", "CROSS"
        BoundExprPtr condition;
        idx_t left_col_count = 0;
        idx_t right_col_count = 0;
    };
    std::vector<std::unique_ptr<JoinInfo>> joins;

    // Resolved select list.
    std::vector<BoundExprPtr> select_list;
    // Column names for result set.
    std::vector<std::string> result_names;
    // Column types for result set.
    std::vector<LogicalType> result_types;

    BoundExprPtr where_clause;
    std::vector<BoundExprPtr> group_by;
    BoundExprPtr having_clause;
    std::vector<BoundOrderBy> order_by;
    int64_t limit_count = -1;   // -1 = no limit
    int64_t offset_count = 0;

    // Aggregate functions extracted from select list / having.
    std::vector<BoundFunction *> aggregates;
    bool has_aggregation = false;
    bool has_window = false;
    BoundExprPtr qualify_clause;
};

class BoundCreateTableStatement : public BoundStatement {
public:
    BoundCreateTableStatement() : BoundStatement(BoundStatementType::CREATE_TABLE) {}

    std::string table_name;
    std::vector<ColumnDefinition> columns;
    bool if_not_exists = false;
};

class BoundDropTableStatement : public BoundStatement {
public:
    BoundDropTableStatement() : BoundStatement(BoundStatementType::DROP_TABLE) {}

    std::string table_name;
    bool if_exists = false;
};

class BoundInsertStatement : public BoundStatement {
public:
    BoundInsertStatement() : BoundStatement(BoundStatementType::INSERT) {}

    TableCatalogEntry *table = nullptr;
    // Each row is a vector of bound expressions (constants).
    std::vector<std::vector<BoundExprPtr>> values;
};

struct BoundUpdateAssignment {
    idx_t column_index;
    BoundExprPtr value;
};

class BoundUpdateStatement : public BoundStatement {
public:
    BoundUpdateStatement() : BoundStatement(BoundStatementType::UPDATE) {}

    TableCatalogEntry *table = nullptr;
    std::vector<BoundUpdateAssignment> assignments;
    BoundExprPtr where_clause;
};

class BoundDeleteStatement : public BoundStatement {
public:
    BoundDeleteStatement() : BoundStatement(BoundStatementType::DELETE_STMT) {}

    TableCatalogEntry *table = nullptr;
    BoundExprPtr where_clause;
};

} // namespace slothdb

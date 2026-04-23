#pragma once

#include "slothdb/parser/expression/parsed_expression.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace slothdb {

enum class StatementType : uint8_t {
    SELECT,
    CREATE_TABLE,
    DROP_TABLE,
    INSERT,
    UPDATE,
    DELETE_STMT,
    EXPLAIN,
    DESCRIBE,
    CREATE_VIEW,
    TRUNCATE,
    ALTER_TABLE,
    MERGE,
    COPY,
    BEGIN_TXN,
    COMMIT_TXN,
    ROLLBACK_TXN,
    INVALID
};

// Base class for parsed SQL statements.
class ParsedStatement {
public:
    explicit ParsedStatement(StatementType type) : type_(type) {}
    virtual ~ParsedStatement() = default;

    StatementType GetType() const { return type_; }

private:
    StatementType type_;
};

using ParsedStmtPtr = std::unique_ptr<ParsedStatement>;

// Table reference in FROM clause.
struct TableRef {
    std::string table_name;
    std::string alias;
    // For table functions like GENERATE_SERIES.
    bool is_table_function = false;
    std::vector<ParsedExprPtr> function_args;
    // For joins:
    std::string join_type; // "", "INNER", "LEFT", "RIGHT", "FULL", "CROSS"
    std::unique_ptr<TableRef> right;
    ParsedExprPtr on_condition;
};

// ORDER BY item.
struct OrderByItem {
    ParsedExprPtr expression;
    bool ascending = true;
    bool nulls_first = false;
};

// SELECT statement.
class SelectStatement : public ParsedStatement {
public:
    SelectStatement() : ParsedStatement(StatementType::SELECT) {}

    bool is_distinct = false;
    std::vector<ParsedExprPtr> select_list;
    std::unique_ptr<TableRef> from_table;
    ParsedExprPtr where_clause;
    std::vector<ParsedExprPtr> group_by;
    ParsedExprPtr having_clause;
    ParsedExprPtr qualify_clause;   // Snowflake-style QUALIFY
    std::vector<OrderByItem> order_by;
    ParsedExprPtr limit;
    ParsedExprPtr offset;

    // Set operations: UNION [ALL], INTERSECT, EXCEPT.
    std::string set_op; // "", "UNION", "UNION ALL", "INTERSECT", "EXCEPT"
    std::unique_ptr<SelectStatement> set_right;

    // CTEs: WITH name AS (SELECT ...) ...
    struct CTE {
        std::string name;
        std::unique_ptr<SelectStatement> query;
        bool recursive = false;
        std::string original_sql; // for re-executing through Connection::Query()
    };
    std::vector<CTE> ctes;
};

// Column definition in CREATE TABLE.
struct ParsedColumnDef {
    std::string name;
    std::string type_name;
    bool not_null = false;
    bool is_primary_key = false;
};

// CREATE TABLE statement.
class CreateTableStatement : public ParsedStatement {
public:
    CreateTableStatement() : ParsedStatement(StatementType::CREATE_TABLE) {}

    std::string table_name;
    std::vector<ParsedColumnDef> columns;
    bool if_not_exists = false;
};

// DROP TABLE statement.
class DropTableStatement : public ParsedStatement {
public:
    DropTableStatement() : ParsedStatement(StatementType::DROP_TABLE) {}

    std::string table_name;
    bool if_exists = false;
};

// INSERT INTO statement.
class InsertStatement : public ParsedStatement {
public:
    InsertStatement() : ParsedStatement(StatementType::INSERT) {}

    std::string table_name;
    std::vector<std::string> column_names; // optional explicit columns
    std::vector<std::vector<ParsedExprPtr>> values; // VALUES (...), (...)
    std::unique_ptr<SelectStatement> select_source;  // INSERT INTO ... SELECT
};

// UPDATE statement.
struct UpdateAssignment {
    std::string column_name;
    ParsedExprPtr value;
};

class UpdateStatement : public ParsedStatement {
public:
    UpdateStatement() : ParsedStatement(StatementType::UPDATE) {}

    std::string table_name;
    std::vector<UpdateAssignment> assignments;
    ParsedExprPtr where_clause;
};

// DELETE statement.
class DeleteStatement : public ParsedStatement {
public:
    DeleteStatement() : ParsedStatement(StatementType::DELETE_STMT) {}

    std::string table_name;
    ParsedExprPtr where_clause;
};

// EXPLAIN statement.
class ExplainStatement : public ParsedStatement {
public:
    ExplainStatement() : ParsedStatement(StatementType::EXPLAIN) {}
    ParsedStmtPtr inner;
};

// DESCRIBE statement: DESCRIBE <query> returns the result schema as rows.
class DescribeStatement : public ParsedStatement {
public:
    DescribeStatement() : ParsedStatement(StatementType::DESCRIBE) {}
    ParsedStmtPtr inner;
};

// CREATE VIEW statement.
class CreateViewStatement : public ParsedStatement {
public:
    CreateViewStatement() : ParsedStatement(StatementType::CREATE_VIEW) {}
    std::string view_name;
    std::unique_ptr<SelectStatement> query;
    bool or_replace = false;
    std::string original_sql; // The original SELECT SQL for virtual view re-execution.
};

// TRUNCATE TABLE statement.
class TruncateStatement : public ParsedStatement {
public:
    TruncateStatement() : ParsedStatement(StatementType::TRUNCATE) {}
    std::string table_name;
};

// ALTER TABLE statement.
class AlterTableStatement : public ParsedStatement {
public:
    AlterTableStatement() : ParsedStatement(StatementType::ALTER_TABLE) {}
    std::string table_name;

    enum class AlterAction { ADD_COLUMN, DROP_COLUMN, RENAME_COLUMN };
    AlterAction action;
    std::string column_name;
    std::string new_column_name; // for RENAME
    std::string column_type;     // for ADD
};

// COPY statement.
class CopyStatement : public ParsedStatement {
public:
    CopyStatement() : ParsedStatement(StatementType::COPY) {}
    std::string table_name;
    std::string file_path;
    bool is_from = false;   // true = COPY FROM (import), false = COPY TO (export)
    std::string format = "CSV"; // CSV, JSON, PARQUET
    char delimiter = ',';
    bool header = true;
    // Optional subquery: COPY (SELECT ...) TO 'file' — only valid for export.
    std::unique_ptr<SelectStatement> source_query;
};

// Transaction control statements.
class BeginStatement : public ParsedStatement {
public:
    BeginStatement() : ParsedStatement(StatementType::BEGIN_TXN) {}
};

class CommitStatement : public ParsedStatement {
public:
    CommitStatement() : ParsedStatement(StatementType::COMMIT_TXN) {}
};

class RollbackStatement : public ParsedStatement {
public:
    RollbackStatement() : ParsedStatement(StatementType::ROLLBACK_TXN) {}
};

// MERGE statement.
class MergeStatement : public ParsedStatement {
public:
    MergeStatement() : ParsedStatement(StatementType::MERGE) {}
    std::string target_table;
    std::string target_alias;
    std::string source_table;
    std::string source_alias;
    ParsedExprPtr on_condition;

    // WHEN MATCHED THEN UPDATE SET ...
    bool has_update = false;
    std::vector<UpdateAssignment> update_assignments;

    // WHEN NOT MATCHED THEN INSERT (cols) VALUES (vals)
    bool has_insert = false;
    std::vector<std::string> insert_columns;
    std::vector<ParsedExprPtr> insert_values;
};

} // namespace slothdb

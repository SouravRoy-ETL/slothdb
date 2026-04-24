#pragma once

#include <string>
#include <vector>

namespace slothdb {
namespace ask {

// Column snapshot for schema resolution. Matches the shape the shell
// can build from `information_schema.columns` without pulling in the
// full Catalog object (keeps this module decoupled from the catalog).
struct Column {
    std::string name;
    std::string type; // e.g. "INTEGER", "VARCHAR", "DOUBLE"
};

struct Table {
    std::string name;
    std::vector<Column> columns;
    // Non-empty when this "table" is actually a file the shell peeked
    // the schema of (e.g. DESCRIBE 'C:\path\file.csv'). The LLM prompt
    // will tell the model to query the file via `FROM '<source_file>'`
    // and the post-processor rewrites any FROM <alias> back to the
    // quoted file literal.
    std::string source_file;
};

struct Schema {
    std::vector<Table> tables;
};

enum class Status {
    OK,                  // translation succeeded, `sql` populated
    NO_MATCH,            // no rule matched the phrasing
    UNRESOLVED_TABLE,    // couldn't map noun to a table in the schema
    UNRESOLVED_COLUMN,   // couldn't map noun to a column
    AMBIGUOUS,           // phrasing matched more than one table/column
};

struct Result {
    Status status = Status::NO_MATCH;
    std::string sql;         // populated when status == OK
    std::string message;     // human-readable explanation for non-OK statuses
    std::string unresolved;  // the specific token that couldn't be resolved
};

// Translate an English question to SQL against the given schema. Pure
// function, no allocations beyond the result. Rules-only - no model
// inference, no catalog dependencies, no I/O. Deterministic: same
// inputs always produce the same output.
Result Translate(const std::string &nl_query, const Schema &schema);

} // namespace ask
} // namespace slothdb

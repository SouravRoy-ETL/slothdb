#pragma once

#include <memory>
#include <string>
#include <vector>

namespace slothdb {

class Catalog;
struct TableRef;
class SelectStatement;
class DataTable;

// Materialise a SelectStatement (incl. VALUES / set-op chains) into a
// freshly-created catalog table named `dest_name`, returning its storage.
std::shared_ptr<DataTable> MaterializeSelectIntoTable(
    Catalog &catalog, SelectStatement &sel, const std::string &dest_name);

// Recursively materialise derived-table FROM sources (`FROM (SELECT..)`,
// `FROM (VALUES..)`) into temp catalog tables, rewriting each TableRef to
// the temp table name. Appends created names to `created` for cleanup.
// Shared by Connection::Query and ExpressionExecutor::ExecuteSubquery.
void MaterializeFromSubqueries(Catalog &catalog, TableRef &tref,
                               std::vector<std::string> &created);

} // namespace slothdb

#pragma once

#include "slothdb/planner/logical_operator.hpp"
#include "slothdb/binder/bound_statement.hpp"

namespace slothdb {

// Converts bound statements into logical operator trees.
class Planner {
public:
    static LogicalOpPtr Plan(const BoundStatement &stmt);

private:
    static LogicalOpPtr PlanSelect(const BoundSelectStatement &stmt);
    static LogicalOpPtr PlanInsert(const BoundInsertStatement &stmt);
    static LogicalOpPtr PlanCreateTable(const BoundCreateTableStatement &stmt);
    static LogicalOpPtr PlanDropTable(const BoundDropTableStatement &stmt);
    static LogicalOpPtr PlanUpdate(const BoundUpdateStatement &stmt);
    static LogicalOpPtr PlanDelete(const BoundDeleteStatement &stmt);
};

} // namespace slothdb

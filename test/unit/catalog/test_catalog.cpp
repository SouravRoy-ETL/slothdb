#include "doctest.h"
#include "slothdb/catalog/catalog.hpp"
#include "slothdb/common/exception.hpp"

using namespace slothdb;

TEST_CASE("Catalog - default schema exists") {
    Catalog catalog;
    CHECK(catalog.SchemaExists("main"));
}

TEST_CASE("Catalog - create and get table") {
    Catalog catalog;
    std::vector<ColumnDefinition> cols = {
        {"id", LogicalType::INTEGER()},
        {"name", LogicalType::VARCHAR()},
    };

    auto &entry = catalog.CreateTable("users", cols);
    CHECK(entry.GetName() == "users");
    CHECK(entry.ColumnCount() == 2);
    CHECK(entry.GetColumns()[0].name == "id");
    CHECK(entry.GetColumns()[1].name == "name");

    auto *found = catalog.GetTable("users");
    CHECK(found != nullptr);
    CHECK(found->GetName() == "users");
}

TEST_CASE("Catalog - table not found returns nullptr") {
    Catalog catalog;
    CHECK(catalog.GetTable("nonexistent") == nullptr);
}

TEST_CASE("Catalog - duplicate table throws") {
    Catalog catalog;
    std::vector<ColumnDefinition> cols = {{"id", LogicalType::INTEGER()}};
    catalog.CreateTable("t1", cols);
    CHECK_THROWS_AS(catalog.CreateTable("t1", cols), CatalogException);
}

TEST_CASE("Catalog - drop table") {
    Catalog catalog;
    std::vector<ColumnDefinition> cols = {{"id", LogicalType::INTEGER()}};
    catalog.CreateTable("t1", cols);

    CHECK(catalog.DropTable("t1") == true);
    CHECK(catalog.GetTable("t1") == nullptr);
    CHECK(catalog.DropTable("t1") == false); // already dropped
}

TEST_CASE("Catalog - create schema") {
    Catalog catalog;
    catalog.CreateSchema("analytics");
    CHECK(catalog.SchemaExists("analytics"));

    std::vector<ColumnDefinition> cols = {{"x", LogicalType::DOUBLE()}};
    catalog.CreateTable("metrics", cols, "analytics");

    auto *found = catalog.GetTable("metrics", "analytics");
    CHECK(found != nullptr);
    CHECK(found->GetName() == "metrics");
}

TEST_CASE("Catalog - column index lookup") {
    Catalog catalog;
    std::vector<ColumnDefinition> cols = {
        {"a", LogicalType::INTEGER()},
        {"b", LogicalType::VARCHAR()},
        {"c", LogicalType::DOUBLE()},
    };
    auto &entry = catalog.CreateTable("t", cols);

    CHECK(entry.GetColumnIndex("a") == 0);
    CHECK(entry.GetColumnIndex("b") == 1);
    CHECK(entry.GetColumnIndex("c") == 2);
    CHECK(entry.GetColumnIndex("z") == INVALID_INDEX);
}

TEST_CASE("Catalog - GetTypes") {
    Catalog catalog;
    std::vector<ColumnDefinition> cols = {
        {"id", LogicalType::INTEGER()},
        {"val", LogicalType::DOUBLE()},
    };
    auto &entry = catalog.CreateTable("t", cols);

    auto types = entry.GetTypes();
    CHECK(types.size() == 2);
    CHECK(types[0].id() == LogicalTypeId::INTEGER);
    CHECK(types[1].id() == LogicalTypeId::DOUBLE);
}

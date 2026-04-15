#include "doctest.h"
#include "slothdb/extension/extension_manager.hpp"
#include "slothdb/extension/extension_api.h"

using namespace slothdb;

// ============================================================================
// Built-in extension function for testing (no DLL loading needed).
// ============================================================================

// Example: double_it(x) -> x * 2
static slothdb_ext_value *ext_double_it(const slothdb_ext_func_args *args) {
    if (args->argc < 1 || slothdb_ext_value_is_null(args->argv[0])) {
        return slothdb_ext_value_null();
    }
    int32_t val = slothdb_ext_value_get_int32(args->argv[0]);
    return slothdb_ext_value_int32(val * 2);
}

// Example: greet(name) -> "Hello, <name>!"
static slothdb_ext_value *ext_greet(const slothdb_ext_func_args *args) {
    if (args->argc < 1 || slothdb_ext_value_is_null(args->argv[0])) {
        return slothdb_ext_value_varchar("Hello, stranger!");
    }
    const char *name = slothdb_ext_value_get_varchar(args->argv[0]);
    std::string result = std::string("Hello, ") + name + "!";
    return slothdb_ext_value_varchar(result.c_str());
}

// Example: add_values(a, b) -> a + b
static slothdb_ext_value *ext_add(const slothdb_ext_func_args *args) {
    if (args->argc < 2) return slothdb_ext_value_null();
    int64_t a = slothdb_ext_value_get_int64(args->argv[0]);
    int64_t b = slothdb_ext_value_get_int64(args->argv[1]);
    return slothdb_ext_value_int64(a + b);
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("Extension - register and execute scalar function") {
    ExtensionManager mgr;

    ExtensionFunction func;
    func.name = "DOUBLE_IT";
    func.num_args = 1;
    func.c_func = ext_double_it;
    func.return_type = SLOTHDB_EXT_TYPE_INTEGER;
    mgr.RegisterScalarFunction(func);

    auto *found = mgr.FindFunction("DOUBLE_IT");
    CHECK(found != nullptr);
    CHECK(found->name == "DOUBLE_IT");

    auto result = mgr.ExecuteFunction("DOUBLE_IT", {Value::INTEGER(21)});
    CHECK(result.GetValue<int32_t>() == 42);
}

TEST_CASE("Extension - string function") {
    ExtensionManager mgr;

    ExtensionFunction func;
    func.name = "GREET";
    func.num_args = 1;
    func.c_func = ext_greet;
    func.return_type = SLOTHDB_EXT_TYPE_VARCHAR;
    mgr.RegisterScalarFunction(func);

    auto result = mgr.ExecuteFunction("GREET", {Value::VARCHAR("World")});
    CHECK(result.GetValue<std::string>() == "Hello, World!");
}

TEST_CASE("Extension - multi-arg function") {
    ExtensionManager mgr;

    ExtensionFunction func;
    func.name = "ADD_VALUES";
    func.num_args = 2;
    func.c_func = ext_add;
    func.return_type = SLOTHDB_EXT_TYPE_BIGINT;
    mgr.RegisterScalarFunction(func);

    auto result = mgr.ExecuteFunction("ADD_VALUES",
        {Value::INTEGER(30), Value::INTEGER(12)});
    CHECK(result.GetValue<int64_t>() == 42);
}

TEST_CASE("Extension - null handling") {
    ExtensionManager mgr;

    ExtensionFunction func;
    func.name = "DOUBLE_IT";
    func.num_args = 1;
    func.c_func = ext_double_it;
    func.return_type = SLOTHDB_EXT_TYPE_INTEGER;
    mgr.RegisterScalarFunction(func);

    auto result = mgr.ExecuteFunction("DOUBLE_IT", {Value()});
    CHECK(result.IsNull());
}

TEST_CASE("Extension - list functions") {
    ExtensionManager mgr;

    ExtensionFunction f1;
    f1.name = "FUNC_A";
    f1.c_func = ext_double_it;
    f1.return_type = SLOTHDB_EXT_TYPE_INTEGER;
    mgr.RegisterScalarFunction(f1);

    ExtensionFunction f2;
    f2.name = "FUNC_B";
    f2.c_func = ext_greet;
    f2.return_type = SLOTHDB_EXT_TYPE_VARCHAR;
    mgr.RegisterScalarFunction(f2);

    auto funcs = mgr.ListFunctions();
    CHECK(funcs.size() == 2);
}

TEST_CASE("Extension - function not found throws") {
    ExtensionManager mgr;
    CHECK_THROWS(mgr.ExecuteFunction("NONEXISTENT", {}));
}

TEST_CASE("Extension - C ABI value creation and access") {
    auto *v1 = slothdb_ext_value_int32(42);
    CHECK(slothdb_ext_value_get_int32(v1) == 42);
    CHECK(slothdb_ext_value_is_null(v1) == 0);
    slothdb_ext_value_free(v1);

    auto *v2 = slothdb_ext_value_varchar("test");
    CHECK(std::string(slothdb_ext_value_get_varchar(v2)) == "test");
    slothdb_ext_value_free(v2);

    auto *v3 = slothdb_ext_value_double(3.14);
    CHECK(slothdb_ext_value_get_double(v3) == doctest::Approx(3.14));
    slothdb_ext_value_free(v3);

    auto *v4 = slothdb_ext_value_null();
    CHECK(slothdb_ext_value_is_null(v4) == 1);
    slothdb_ext_value_free(v4);

    auto *v5 = slothdb_ext_value_boolean(1);
    CHECK(slothdb_ext_value_get_boolean(v5) == 1);
    slothdb_ext_value_free(v5);
}

TEST_CASE("Extension - register via manager API") {
    ExtensionManager mgr;

    ExtensionFunction func;
    func.name = "EXT_DOUBLE";
    func.num_args = 1;
    func.c_func = ext_double_it;
    func.return_type = SLOTHDB_EXT_TYPE_INTEGER;
    mgr.RegisterScalarFunction(func);

    auto *found = mgr.FindFunction("EXT_DOUBLE");
    CHECK(found != nullptr);

    auto val = mgr.ExecuteFunction("EXT_DOUBLE", {Value::INTEGER(5)});
    CHECK(val.GetValue<int32_t>() == 10);
}

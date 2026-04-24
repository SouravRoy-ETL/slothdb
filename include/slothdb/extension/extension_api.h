/*
 * SlothDB Extension API (Stable C ABI)
 *
 * This is the public API for building SlothDB extensions.
 * Extensions are dynamic shared libraries (.dll/.so/.dylib) that register
 * new functions, types, table functions, and file formats.
 *
 * ABI STABILITY GUARANTEE:
 * - This API is versioned. Extensions compiled against version N will
 *   work with SlothDB version N and later (backward compatible).
 * - The API uses only C types (no C++ classes, templates, or STL).
 * - All memory is allocated/freed on the same side of the boundary.
 *
 * BUILDING AN EXTENSION:
 *   #include "slothdb/extension/extension_api.h"
 *
 *   SLOTHDB_EXTENSION_ENTRY(my_extension) {
 *       slothdb_ext_register_scalar_function(info, "my_func", 1, my_func_impl);
 *       return SLOTHDB_EXT_OK;
 *   }
 */

#ifndef SLOTHDB_EXTENSION_API_H
#define SLOTHDB_EXTENSION_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* API version - bump major for breaking changes, minor for additions. */
#define SLOTHDB_EXT_API_VERSION_MAJOR 1
#define SLOTHDB_EXT_API_VERSION_MINOR 0

/* Extension status codes. */
#define SLOTHDB_EXT_OK       0
#define SLOTHDB_EXT_ERROR    1
#define SLOTHDB_EXT_VERSION_MISMATCH 2

/* Value types (matches LogicalTypeId). */
typedef enum {
    SLOTHDB_EXT_TYPE_NULL = 0,
    SLOTHDB_EXT_TYPE_BOOLEAN = 2,
    SLOTHDB_EXT_TYPE_INTEGER = 5,
    SLOTHDB_EXT_TYPE_BIGINT = 6,
    SLOTHDB_EXT_TYPE_FLOAT = 11,
    SLOTHDB_EXT_TYPE_DOUBLE = 12,
    SLOTHDB_EXT_TYPE_VARCHAR = 15,
} slothdb_ext_type;

/* Opaque value handle. */
typedef struct slothdb_ext_value slothdb_ext_value;

/* Value creation. */
slothdb_ext_value *slothdb_ext_value_null(void);
slothdb_ext_value *slothdb_ext_value_int32(int32_t val);
slothdb_ext_value *slothdb_ext_value_int64(int64_t val);
slothdb_ext_value *slothdb_ext_value_double(double val);
slothdb_ext_value *slothdb_ext_value_varchar(const char *val);
slothdb_ext_value *slothdb_ext_value_boolean(int val);

/* Value access. */
int slothdb_ext_value_is_null(const slothdb_ext_value *val);
int32_t slothdb_ext_value_get_int32(const slothdb_ext_value *val);
int64_t slothdb_ext_value_get_int64(const slothdb_ext_value *val);
double slothdb_ext_value_get_double(const slothdb_ext_value *val);
const char *slothdb_ext_value_get_varchar(const slothdb_ext_value *val);
int slothdb_ext_value_get_boolean(const slothdb_ext_value *val);

/* Free a value. */
void slothdb_ext_value_free(slothdb_ext_value *val);

/* ========================================================================
 * Function arguments and results
 * ======================================================================== */

typedef struct slothdb_ext_func_args {
    int argc;
    const slothdb_ext_value **argv;
} slothdb_ext_func_args;

/* Scalar function signature: takes args, returns a value. */
typedef slothdb_ext_value *(*slothdb_ext_scalar_func)(const slothdb_ext_func_args *args);

/* ========================================================================
 * Extension info (passed to the init function)
 * ======================================================================== */

typedef struct slothdb_ext_info slothdb_ext_info;

/* Register a scalar function.
 * name: function name (SQL-callable)
 * num_args: number of arguments (-1 for variadic)
 * func: function pointer
 * return_type: return type
 */
int slothdb_ext_register_scalar_function(
    slothdb_ext_info *info,
    const char *name,
    int num_args,
    slothdb_ext_scalar_func func,
    slothdb_ext_type return_type
);

/* Register a scalar function with a description. */
int slothdb_ext_register_scalar_function_with_desc(
    slothdb_ext_info *info,
    const char *name,
    int num_args,
    slothdb_ext_scalar_func func,
    slothdb_ext_type return_type,
    const char *description
);

/* ========================================================================
 * Extension entry point
 * ======================================================================== */

/* Extension init function signature. */
typedef int (*slothdb_ext_init_func)(slothdb_ext_info *info);

/* Helper macro to define the extension entry point. */
#ifdef _MSC_VER
#define SLOTHDB_EXTENSION_EXPORT __declspec(dllexport)
#else
#define SLOTHDB_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

#define SLOTHDB_EXTENSION_ENTRY(name) \
    SLOTHDB_EXTENSION_EXPORT int slothdb_extension_init_##name(slothdb_ext_info *info)

/* Standard entry point name. */
#define SLOTHDB_EXTENSION_INIT_FUNC "slothdb_extension_init"

/* Version check function - extensions should export this. */
#define SLOTHDB_EXTENSION_VERSION_FUNC "slothdb_extension_version"
typedef int (*slothdb_ext_version_func)(int *major, int *minor);

#ifdef __cplusplus
}
#endif

#endif /* SLOTHDB_EXTENSION_API_H */

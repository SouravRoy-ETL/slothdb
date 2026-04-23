/*
 * SlothDB C API
 *
 * This is the public C API for embedding SlothDB in applications.
 * All language bindings (Python, Java, etc.) should use this API.
 */

#ifndef SLOTHDB_H
#define SLOTHDB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles. */
typedef struct slothdb_database slothdb_database;
typedef struct slothdb_connection slothdb_connection;
typedef struct slothdb_result slothdb_result;

/* Error codes. */
typedef enum {
    SLOTHDB_OK = 0,
    SLOTHDB_ERROR = 1,
    SLOTHDB_INVALID = 2,
} slothdb_status;

/* Column type IDs (matches LogicalTypeId). */
typedef enum {
    SLOTHDB_TYPE_INVALID = 0,
    SLOTHDB_TYPE_BOOLEAN = 2,
    SLOTHDB_TYPE_INTEGER = 5,
    SLOTHDB_TYPE_BIGINT = 6,
    SLOTHDB_TYPE_FLOAT = 11,
    SLOTHDB_TYPE_DOUBLE = 12,
    SLOTHDB_TYPE_VARCHAR = 15,
} slothdb_type;

/* ========================================================================
 * Database lifecycle
 * ======================================================================== */

/* Open a database. path="" or NULL for in-memory. */
slothdb_status slothdb_open(const char *path, slothdb_database **out_db);

/* Close and free a database. */
void slothdb_close(slothdb_database *db);

/* ========================================================================
 * Connections
 * ======================================================================== */

/* Create a connection to a database. */
slothdb_status slothdb_connect(slothdb_database *db, slothdb_connection **out_conn);

/* Close and free a connection. */
void slothdb_disconnect(slothdb_connection *conn);

/* ========================================================================
 * Query execution
 * ======================================================================== */

/* Execute a SQL query. Result must be freed with slothdb_free_result. */
slothdb_status slothdb_query(slothdb_connection *conn, const char *sql,
                              slothdb_result **out_result);

/* Get the error message from the last failed operation. */
const char *slothdb_result_error(slothdb_result *result);

/* ========================================================================
 * Result inspection
 * ======================================================================== */

/* Number of columns in the result. */
uint64_t slothdb_column_count(slothdb_result *result);

/* Number of rows in the result. */
uint64_t slothdb_row_count(slothdb_result *result);

/* Column name by index. */
const char *slothdb_column_name(slothdb_result *result, uint64_t col);

/* Column type by index. */
slothdb_type slothdb_column_type(slothdb_result *result, uint64_t col);

/* Check if a value is NULL. */
int slothdb_value_is_null(slothdb_result *result, uint64_t row, uint64_t col);

/* Get value as int32. */
int32_t slothdb_value_int32(slothdb_result *result, uint64_t row, uint64_t col);

/* Get value as int64. */
int64_t slothdb_value_int64(slothdb_result *result, uint64_t row, uint64_t col);

/* Get value as double. */
double slothdb_value_double(slothdb_result *result, uint64_t row, uint64_t col);

/* Get value as string. Returned pointer is valid until result is freed. */
const char *slothdb_value_varchar(slothdb_result *result, uint64_t row, uint64_t col);

/* Free a result. */
void slothdb_free_result(slothdb_result *result);

/* ========================================================================
 * Catalog introspection
 *
 * Lets a host iterate tables and their columns without running
 * information_schema SQL. Used by the shell's `.ask` command to build
 * a schema snapshot for natural-language-to-SQL translation, but it's
 * a first-class C API — any binding can call it.
 *
 * Returned pointers are valid only until the next call TO THE SAME
 * FUNCTION on the same thread. Copy the string if you need to keep it
 * across calls. Do not free them.
 * ======================================================================== */

/* Number of tables in the default schema. */
uint64_t slothdb_table_count(slothdb_connection *conn);

/* Table name at index `i` (0 <= i < slothdb_table_count). */
const char *slothdb_table_name(slothdb_connection *conn, uint64_t i);

/* Number of columns in table `i`. */
uint64_t slothdb_table_column_count(slothdb_connection *conn, uint64_t table_index);

/* Column name at (table_index, col_index). */
const char *slothdb_table_column_name(slothdb_connection *conn,
                                       uint64_t table_index, uint64_t col_index);

/* Column type name (e.g. "INTEGER", "VARCHAR") at (table_index, col_index). */
const char *slothdb_table_column_type(slothdb_connection *conn,
                                       uint64_t table_index, uint64_t col_index);

/* ========================================================================
 * Version info
 * ======================================================================== */

const char *slothdb_version(void);

#ifdef __cplusplus
}
#endif

#endif /* SLOTHDB_H */

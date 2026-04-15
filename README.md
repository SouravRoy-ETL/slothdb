# SlothDB

**An embedded analytical database engine.** Zero dependencies. Single file. Blazing fast.

SlothDB is a production-grade in-process OLAP database written in C++20. It runs inside your application — no server, no setup, no external libraries. Think **DuckDB, but with GPU acceleration, structured errors, and a stable extension API**.

```sql
SELECT department, COUNT(*), AVG(salary)
FROM read_parquet('employees.parquet')
WHERE hire_date > '2020-01-01'
GROUP BY department
ORDER BY AVG(salary) DESC
LIMIT 10;
```

## Why SlothDB?

| Feature | SlothDB | DuckDB | SQLite |
|---------|---------|--------|--------|
| OLAP-optimized columnar storage | Yes | Yes | No |
| GPU acceleration (CUDA + Metal) | Yes | No | No |
| Structured error codes | Yes | No | Partial |
| Stable extension C ABI | Yes | No | Yes |
| QUALIFY clause (Snowflake) | Yes | Yes | No |
| Query files directly | CSV, JSON, Parquet, Arrow, Avro, Excel | CSV, JSON, Parquet | No |
| Zero dependencies | Yes | Yes | Yes |
| Single-file database | `.slothdb` | `.duckdb` | `.sqlite` |

## Quick Start

### Build from source

```bash
git clone https://github.com/user/slothdb.git
cd slothdb
cmake -B build
cmake --build build --config Release
```

### Run the CLI

```bash
./build/src/Release/slothdb_shell

slothdb> SELECT 'Hello, SlothDB!' AS greeting;
greeting
---------------
Hello, SlothDB!
(1 row)
```

### Embed in your C/C++ application

```c
#include "slothdb/api/slothdb.h"

int main() {
    slothdb_database *db;
    slothdb_connection *conn;
    slothdb_result *result;

    slothdb_open("my.slothdb", &db);
    slothdb_connect(db, &conn);

    slothdb_query(conn, "CREATE TABLE t (x INTEGER, y VARCHAR)", &result);
    slothdb_free_result(result);

    slothdb_query(conn, "INSERT INTO t VALUES (42, 'hello')", &result);
    slothdb_free_result(result);

    slothdb_query(conn, "SELECT * FROM t", &result);
    printf("%s = %d\n",
        slothdb_column_name(result, 0),
        slothdb_value_int32(result, 0, 0));  // x = 42
    slothdb_free_result(result);

    slothdb_disconnect(conn);
    slothdb_close(db);  // Auto-saves
}
```

## Features

### SQL Support (130+ features)

**DDL:** `CREATE TABLE`, `DROP TABLE`, `ALTER TABLE` (ADD/DROP/RENAME COLUMN), `CREATE VIEW`, `TRUNCATE`

**DML:** `INSERT`, `UPDATE`, `DELETE`, `MERGE`, `INSERT INTO ... SELECT`, `COPY TO/FROM`

**Queries:** `SELECT`, `DISTINCT`, `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT/OFFSET`, `BETWEEN`, `LIKE`, `ILIKE`, `IN`, `EXISTS`, `CASE WHEN`, `CAST`, `TRY_CAST`

**Joins:** `INNER`, `LEFT`, `RIGHT`, `FULL`, `CROSS`

**Set Operations:** `UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT`

**CTEs:** `WITH ... AS`, `WITH RECURSIVE`

**Window Functions:** `ROW_NUMBER`, `RANK`, `DENSE_RANK`, `NTILE`, `LEAD`, `LAG`, `FIRST_VALUE`, `LAST_VALUE`, `SUM/COUNT/AVG OVER (PARTITION BY ... ORDER BY ...)`

**QUALIFY:** Snowflake-style window function filtering

**Aggregates:** `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, `STRING_AGG`, `STDDEV`, `VARIANCE`, `MEDIAN`, `BOOL_AND`, `BOOL_OR`

**Scalar Functions:**
- String: `LENGTH`, `UPPER`, `LOWER`, `CONCAT`, `||`, `SUBSTRING`, `REPLACE`, `TRIM`, `POSITION`, `LEFT`, `RIGHT`, `LPAD`, `RPAD`, `REVERSE`, `REPEAT`, `STARTS_WITH`, `ENDS_WITH`, `CONTAINS`, `SPLIT_PART`, `INITCAP`
- Math: `ABS`, `CEIL`, `FLOOR`, `ROUND`, `SQRT`, `POWER`, `LOG`, `EXP`, `SIGN`, `PI`, `SIN`, `COS`, `TAN`, `ATAN2`, `DEGREES`, `RADIANS`, `RANDOM`, `LEAST`, `GREATEST`
- Date: `NOW()`, `CURRENT_TIMESTAMP`, `CURRENT_DATE`, `EXTRACT`, `DATE_ADD`, `DATE_DIFF`, `STRFTIME`, `TO_TIMESTAMP`
- Null: `COALESCE`, `NULLIF`
- Regex: `REGEXP_MATCHES`, `REGEXP_REPLACE`, `REGEXP_EXTRACT`

**Transactions:** `BEGIN`, `COMMIT`, `ROLLBACK`

**Meta:** `EXPLAIN`

### File I/O (query any format directly)

```sql
-- CSV
SELECT * FROM read_csv('data.csv');
SELECT * FROM 'data.csv';  -- auto-detect

-- JSON / NDJSON
SELECT * FROM read_json('data.json');

-- Parquet (with row group predicate pushdown)
SELECT * FROM read_parquet('data.parquet');

-- Arrow IPC / Feather
SELECT * FROM read_arrow('data.arrow');

-- Avro
SELECT * FROM read_avro('data.avro');

-- Excel
SELECT * FROM read_xlsx('report.xlsx');

-- SQLite databases
SELECT * FROM sqlite_scan('other.db', 'users');

-- Glob patterns (read multiple files)
SELECT * FROM read_csv('logs/*.csv');
SELECT * FROM read_parquet('data/**/*.parquet');

-- Export to any format
COPY table TO 'output.csv';
COPY table TO 'output.json' WITH (FORMAT JSON);
COPY table TO 'output.parquet' WITH (FORMAT PARQUET);
```

### Persistence

```cpp
// Data persists across sessions
{
    Database db("analytics.slothdb");
    Connection conn(db);
    conn.Query("CREATE TABLE metrics (ts BIGINT, value DOUBLE)");
    conn.Query("INSERT INTO metrics VALUES (1000, 3.14)");
} // Auto-saved

{
    Database db("analytics.slothdb");
    Connection conn(db);
    auto r = conn.Query("SELECT * FROM metrics");
    // Data is still here!
}
```

### GPU Acceleration

SlothDB can offload heavy computations to the GPU:

- **NVIDIA CUDA**: For Linux/Windows with NVIDIA GPUs
- **Apple Metal**: For macOS with Apple Silicon (M1/M2/M3/M4)

```bash
# Build with CUDA
cmake -B build -DSLOTHDB_CUDA=ON

# Build with Metal (macOS)
cmake -B build -DSLOTHDB_METAL=ON
```

GPU is used automatically for operations on >100K rows. CPU fallback for smaller datasets.

### Extension System

Build custom extensions with a stable C ABI:

```c
#include "slothdb/extension/extension_api.h"

static slothdb_ext_value *my_func(const slothdb_ext_func_args *args) {
    int32_t x = slothdb_ext_value_get_int32(args->argv[0]);
    return slothdb_ext_value_int32(x * x);
}

SLOTHDB_EXTENSION_ENTRY(my_extension) {
    slothdb_ext_register_scalar_function(info, "SQUARE", 1,
        my_func, SLOTHDB_EXT_TYPE_INTEGER);
    return SLOTHDB_EXT_OK;
}
```

Extensions are backward-compatible — an extension compiled for API v1.0 will work with SlothDB v1.1, v1.2, etc.

## Architecture

```
SQL String
    |
    v
 Parser (hand-written recursive descent)
    |
    v
 Binder (name resolution, type inference)
    |
    v
 Logical Planner (operator tree)
    |
    v
 Optimizer (constant folding, filter pushdown, TopN)
    |
    v
 Physical Planner (execution operators)
    |
    v
 Executor (vectorized, parallel, GPU-accelerated)
    |
    v
 Results
```

**Storage:** Columnar format with row groups (122,880 rows each), compression (RLE, dictionary, bitpacking), zone maps for scan skipping.

**Execution:** Vectorized engine processing 2,048 values per batch. Morsel-driven parallelism across CPU cores. Optional GPU offload for heavy operations.

## Building

### Prerequisites

- C++20 compiler (MSVC 2019+, GCC 12+, Clang 15+, AppleClang 14+)
- CMake 3.14+

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `SLOTHDB_BUILD_TESTS` | ON | Build test suite |
| `SLOTHDB_BUILD_SHELL` | OFF | Build CLI shell |
| `SLOTHDB_CUDA` | OFF | Enable CUDA GPU acceleration |
| `SLOTHDB_METAL` | OFF | Enable Metal GPU acceleration (macOS) |
| `SLOTHDB_SANITIZERS` | OFF | Enable ASan/UBSan |

```bash
# Full build with shell and tests
cmake -B build -DSLOTHDB_BUILD_SHELL=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

### Platform Support

| Platform | Compiler | Status |
|----------|----------|--------|
| Windows 10/11 | MSVC 2019+ | Tested |
| Linux (Ubuntu 20.04+) | GCC 12+ / Clang 15+ | Supported |
| macOS (12+) | AppleClang 14+ | Supported |

## Tests

325 test cases, 131,318 assertions covering:
- Type system and vector engine
- SQL parsing and binding
- Query execution (all operators)
- Aggregation and window functions
- Joins (all types)
- File I/O (CSV, JSON, Parquet, Arrow)
- Persistence (save/load)
- Compression codecs
- Parallel execution
- GPU engine
- Extension system

```bash
# Run all tests
./build/test/Release/slothdb_tests

# Run specific test
./build/test/Release/slothdb_tests -tc="E2E*"
```

## License

MIT

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

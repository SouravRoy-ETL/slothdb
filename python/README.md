<p align="center">
  <h1 align="center">SlothDB</h1>
  <p align="center">
    <b>An embedded analytical database engine</b><br>
    Zero dependencies &middot; Single file &middot; GPU accelerated
  </p>
  <p align="center">
    <a href="https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml"><img src="https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
    <a href="https://github.com/SouravRoy-ETL/slothdb/releases/latest"><img src="https://img.shields.io/github/v/release/SouravRoy-ETL/slothdb?label=release" alt="Release"></a>
    <a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License"></a>
    <a href="https://github.com/SouravRoy-ETL/slothdb/stargazers"><img src="https://img.shields.io/github/stars/SouravRoy-ETL/slothdb?style=social" alt="Stars"></a>
  </p>
</p>

---

SlothDB is a fast, in-process OLAP database for analytics. It runs inside your application with no server, no setup, and no external dependencies. Query CSV, Parquet, JSON, Excel, and more — directly from SQL.

```sql
SELECT department, COUNT(*), AVG(salary)
FROM 'employees.parquet'
WHERE hire_year >= 2020
GROUP BY department
ORDER BY AVG(salary) DESC;
```

## Installation

**CLI Binary** — download from [Releases](https://github.com/SouravRoy-ETL/slothdb/releases/latest):

| Platform | Command |
|----------|---------|
| Windows | Download `slothdb.exe` and run |
| Linux | `chmod +x slothdb && ./slothdb` |
| macOS | `chmod +x slothdb && ./slothdb` |

**Python:**

```bash
pip install slothdb
```

**Build from source:**

```bash
git clone https://github.com/SouravRoy-ETL/slothdb.git
cd slothdb
cmake -B build -DSLOTHDB_BUILD_SHELL=ON
cmake --build build --config Release
./build/src/Release/slothdb
```

## Quick Start

```
$ ./slothdb

slothdb> CREATE TABLE t (name VARCHAR, score INTEGER);
slothdb> INSERT INTO t VALUES ('Alice', 95), ('Bob', 87), ('Charlie', 92);
slothdb> SELECT name, score, RANK() OVER (ORDER BY score DESC) FROM t;
name            | score           | expr
----------------+-----------------+----------------
Alice           | 95              | 1
Charlie         | 92              | 2
Bob             | 87              | 3
```

Query files without importing:

```sql
SELECT * FROM 'data.csv';                              -- CSV
SELECT * FROM read_parquet('logs/*.parquet');           -- Parquet with globs
SELECT * FROM read_json('events.json');                -- JSON
SELECT * FROM read_xlsx('report.xlsx');                -- Excel
SELECT * FROM sqlite_scan('app.db', 'users');          -- SQLite

COPY results TO 'output.parquet' WITH (FORMAT PARQUET); -- Export
```

Persistent database:

```
$ ./slothdb analytics.slothdb    # data saved automatically
```

## Why SlothDB?

SlothDB is designed as a modern alternative to [DuckDB](https://duckdb.org) with additional capabilities:

| | SlothDB | DuckDB |
|-|---------|--------|
| **GPU acceleration** | CUDA + Apple Metal | — |
| **Structured errors** | Stable numeric error codes | Free-form strings |
| **Extension ABI** | Stable C ABI, never breaks | Breaks across versions |
| **File formats** | CSV, Parquet, JSON, Arrow, Avro, Excel, SQLite — all built-in | CSV, Parquet, JSON built-in; others need extensions |
| **QUALIFY** | Yes | Yes |
| **Zero dependencies** | Yes | Yes |

## Python

```python
import slothdb

db = slothdb.connect()                    # in-memory
db = slothdb.connect("analytics.slothdb") # persistent

result = db.sql("""
    SELECT department, COUNT(*), AVG(salary) 
    FROM 'employees.csv' 
    GROUP BY department
""")
print(result)
df = result.fetchdf()  # → pandas DataFrame
```

## C/C++ Embedding

```c
#include "slothdb/api/slothdb.h"

slothdb_database *db;
slothdb_connection *conn;
slothdb_result *result;

slothdb_open("analytics.slothdb", &db);
slothdb_connect(db, &conn);
slothdb_query(conn, "SELECT 42 AS answer", &result);
printf("%d\n", slothdb_value_int32(result, 0, 0));
slothdb_free_result(result);
slothdb_disconnect(conn);
slothdb_close(db);
```

## Features

- **130+ SQL features** — SELECT, JOINs, CTEs, window functions, aggregates, MERGE, EXPLAIN, transactions ([full reference](docs/SQL_REFERENCE.md))
- **QUALIFY clause** — filter on window function results (Snowflake-style)
- **7 file formats** — CSV, JSON, Parquet, Arrow, Avro, Excel, SQLite — all built-in, no extensions
- **GPU acceleration** — CUDA (NVIDIA) and Metal (Apple Silicon) for large-scale analytics
- **Single-file persistence** — `.slothdb` format with auto-save
- **Query optimizer** — constant folding, filter pushdown, TopN optimization
- **Vectorized execution** — columnar engine processing 2,048 values per batch
- **Parallel execution** — morsel-driven parallelism across all CPU cores
- **Compression** — RLE, dictionary, bitpacking with zone maps for scan skipping
- **Extension system** — stable C ABI for third-party extensions
- **325 tests** — 131,000+ assertions across all subsystems

## Documentation

- [SQL Reference](docs/SQL_REFERENCE.md) — complete list of SQL features, functions, and types
- [Sample Data](examples/) — CSV and JSON files to try with SlothDB
- [Contributing](CONTRIBUTING.md) — how to build, test, and submit changes
- [Extension API](include/slothdb/extension/extension_api.h) — build custom extensions

## Development

```bash
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DSLOTHDB_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release    # run 325 tests
```

| Build Option | Description |
|-------------|-------------|
| `-DSLOTHDB_BUILD_SHELL=ON` | Build CLI |
| `-DSLOTHDB_CUDA=ON` | Enable NVIDIA GPU |
| `-DSLOTHDB_METAL=ON` | Enable Apple GPU |
| `-DSLOTHDB_SANITIZERS=ON` | Enable ASan/UBSan |

## License

[MIT](LICENSE)

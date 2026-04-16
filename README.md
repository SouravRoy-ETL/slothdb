<p align="center">
  <img src="assets/logo.svg" width="400" alt="SlothDB Logo">
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

| Platform | Command |
|----------|---------|
| **Linux / macOS** | `curl -fsSL https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/install.sh \| bash` |
| **Ubuntu / Debian** | `sudo dpkg -i slothdb_0.1.0_amd64.deb` ([download .deb](https://github.com/SouravRoy-ETL/slothdb/releases/latest)) |
| **Fedora / RHEL** | `sudo rpm -i slothdb-0.1.0.rpm` (build from [spec](packaging/rpm/slothdb.spec)) |
| **Arch Linux** | `makepkg -si` (use [PKGBUILD](packaging/arch/PKGBUILD)) |
| **macOS (Homebrew)** | `brew install --build-from-source packaging/homebrew/slothdb.rb` |
| **Windows** | Download [`slothdb.exe`](https://github.com/SouravRoy-ETL/slothdb/releases/latest) |
| **Python** | `pip install slothdb` |

Then just run:
```bash
slothdb
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

## Why Switch from DuckDB to SlothDB?

DuckDB is great. SlothDB is what comes next.

### 1. GPU Acceleration — 20-100x faster on large datasets

DuckDB runs on CPU only. SlothDB offloads aggregation, sorting, and filtering to your GPU — **CUDA** on NVIDIA, **Metal** on Apple Silicon. On a 10M-row GROUP BY, that's the difference between 5 seconds and 50 milliseconds.

```sql
-- This runs on GPU automatically when data > 100K rows
SELECT department, COUNT(*), AVG(salary) FROM employees GROUP BY department;
```

### 2. Your Extensions Will Never Break Again

DuckDB extensions break on every release because they depend on internal C++ APIs. Teams waste days fixing extensions after upgrades. SlothDB's **stable C ABI** guarantees backward compatibility — an extension built for v1.0 works on v1.1, v2.0, and beyond. Zero maintenance.

### 3. Errors You Can Actually Handle in Code

DuckDB throws free-form error strings that change between versions. Your error-handling code breaks silently. SlothDB gives every error a **stable numeric code** + category — catch `ErrorCode::TABLE_NOT_FOUND` (2000) instead of parsing `"Table 'foo' not found"`.

```cpp
try { db.sql("SELECT * FROM nonexistent"); }
catch (const SlothDBException &e) {
    if (e.GetCode() == ErrorCode::TABLE_NOT_FOUND) { /* handle */ }
    // Works in v1.0, v2.0, v10.0 — the code never changes.
}
```

### 4. Every File Format Built In — No Extensions to Install

DuckDB requires installing extensions for Excel, Avro, SQLite, and HTTP access. SlothDB ships everything **out of the box**:

```sql
SELECT * FROM 'report.xlsx';                           -- Excel (DuckDB: needs extension)
SELECT * FROM read_avro('events.avro');                -- Avro (DuckDB: needs extension)
SELECT * FROM sqlite_scan('app.db', 'users');          -- SQLite (DuckDB: needs extension)
SELECT * FROM read_csv('data/*.csv');                  -- Glob patterns
```

### 5. QUALIFY — Snowflake's Best Feature, Built In

Filter window function results without subqueries. One query instead of three:

```sql
-- Get the top earner per department — no subquery needed
SELECT name, department, salary
FROM employees
QUALIFY ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) = 1;
```

### Full Comparison

| | SlothDB | DuckDB |
|-|---------|--------|
| **GPU acceleration** | CUDA + Apple Metal (20-100x on large data) | CPU only |
| **Extension stability** | Stable C ABI — never breaks | C++ internal API — breaks every release |
| **Error handling** | Numeric codes, stable across versions | Free-form strings, change between versions |
| **Built-in formats** | CSV, Parquet, JSON, Arrow, Avro, Excel, SQLite | CSV, Parquet, JSON (others need extensions) |
| **QUALIFY clause** | Yes | Yes |
| **Crash-safe persistence** | Atomic checkpoint (write-then-rename) | Yes |
| **Memory safety** | Bounds-checked file parsing, DoS limits | Some unchecked paths |
| **Zero dependencies** | Yes | Yes |
| **SQL features** | 130+ | 130+ |

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

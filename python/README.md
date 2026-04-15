# SlothDB

**An embedded analytical database engine.** Zero dependencies. Single file. GPU accelerated.

[![CI](https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml/badge.svg)](https://github.com/SouravRoy-ETL/slothdb/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Tests](https://img.shields.io/badge/tests-325%20passed-brightgreen)]()
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)]()
[![GitHub release](https://img.shields.io/github/v/release/SouravRoy-ETL/slothdb)](https://github.com/SouravRoy-ETL/slothdb/releases)

---

## Get Started in 30 Seconds

### 1. Download

Go to **[Releases](https://github.com/SouravRoy-ETL/slothdb/releases/latest)** and download for your platform:

| Platform | File | How to run |
|----------|------|------------|
| **Windows** | `slothdb_shell.exe` | Double-click or run in terminal |
| **Linux** | `slothdb_shell` | `chmod +x slothdb_shell && ./slothdb_shell` |
| **macOS** | `slothdb_shell` | `chmod +x slothdb_shell && ./slothdb_shell` |

### 2. Run it

```
$ ./slothdb_shell

SlothDB Shell vSlothDB 0.1.0
Connected to in-memory database

slothdb> SELECT 'Hello World!' AS greeting;
greeting
---------------
Hello World!
(1 row)
```

### 3. Try with sample data

Create a CSV file called `employees.csv`:
```csv
name,department,salary,hire_year
Alice,Engineering,120000,2020
Bob,Engineering,110000,2019
Charlie,Sales,95000,2021
Diana,Sales,105000,2020
Eve,Marketing,98000,2022
Frank,Engineering,130000,2018
Grace,Sales,115000,2019
Hank,Marketing,92000,2023
```

Now query it directly (no import needed!):

```sql
-- Query CSV file directly
slothdb> SELECT * FROM 'employees.csv';

-- Average salary by department
slothdb> SELECT department, COUNT(*) AS headcount, AVG(salary) AS avg_salary
   ...> FROM 'employees.csv'
   ...> GROUP BY department;

-- Top earner per department (QUALIFY - Snowflake feature!)
slothdb> SELECT name, department, salary,
   ...>   ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC)
   ...> FROM 'employees.csv'
   ...> QUALIFY ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) = 1;

-- Export results to Parquet
slothdb> CREATE TABLE emp AS SELECT * FROM 'employees.csv';
slothdb> COPY emp TO 'employees.parquet' WITH (FORMAT PARQUET);

-- Read the Parquet file back
slothdb> SELECT * FROM 'employees.parquet' WHERE salary > 100000;
```

### 4. Use a persistent database

```
$ ./slothdb_shell mydata.slothdb

slothdb> CREATE TABLE users (id INTEGER, name VARCHAR, email VARCHAR);
slothdb> INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');
slothdb> .quit

$ ./slothdb_shell mydata.slothdb
slothdb> SELECT * FROM users;  -- Data is still here!
```

---

## SlothDB vs DuckDB — Side by Side

Both work the same way. If you know DuckDB, you know SlothDB:

| Task | DuckDB | SlothDB |
|------|--------|---------|
| **Start CLI** | `./duckdb` | `./slothdb_shell` |
| **Open database** | `./duckdb mydb.duckdb` | `./slothdb_shell mydb.slothdb` |
| **Query CSV** | `SELECT * FROM 'data.csv'` | `SELECT * FROM 'data.csv'` |
| **Query Parquet** | `SELECT * FROM 'data.parquet'` | `SELECT * FROM 'data.parquet'` |
| **Query JSON** | `SELECT * FROM 'data.json'` | `SELECT * FROM 'data.json'` |
| **Export** | `COPY t TO 'out.parquet'` | `COPY t TO 'out.parquet' WITH (FORMAT PARQUET)` |
| **Window functions** | `ROW_NUMBER() OVER (...)` | `ROW_NUMBER() OVER (...)` |
| **QUALIFY** | `QUALIFY row_num = 1` | `QUALIFY row_num = 1` |

### What SlothDB adds over DuckDB

| Feature | SlothDB | DuckDB |
|---------|---------|--------|
| GPU acceleration (CUDA + Metal) | Built-in | Not available |
| Structured error codes | Every error has a stable numeric code | Free-form strings that change between versions |
| Stable extension C ABI | Extensions never break across versions | Extensions break frequently |
| Read Excel files | `SELECT * FROM 'report.xlsx'` | Requires extension install |
| Read Avro files | `SELECT * FROM read_avro('data.avro')` | Requires extension install |
| Read SQLite databases | `SELECT * FROM sqlite_scan('db', 'table')` | Requires extension install |

---

## Python

```bash
pip install slothdb
```

```python
import slothdb

# Connect (in-memory or persistent)
db = slothdb.connect()
# db = slothdb.connect("analytics.slothdb")

# Run SQL
result = db.sql("SELECT 42 AS answer, 'hello' AS greeting")
print(result)
# answer     | greeting
# -----------+-----------
# 42         | hello

# Query files
result = db.sql("SELECT * FROM 'employees.csv' WHERE salary > 100000")

# Convert to pandas
df = result.fetchdf()
print(df)
```

---

## C/C++ Embedding

```c
#include "slothdb/api/slothdb.h"

int main() {
    slothdb_database *db;
    slothdb_connection *conn;
    slothdb_result *result;

    slothdb_open("analytics.slothdb", &db);
    slothdb_connect(db, &conn);

    slothdb_query(conn, "CREATE TABLE t (x INTEGER, name VARCHAR)", &result);
    slothdb_free_result(result);

    slothdb_query(conn, "INSERT INTO t VALUES (42, 'hello')", &result);
    slothdb_free_result(result);

    slothdb_query(conn, "SELECT * FROM t", &result);
    for (uint64_t r = 0; r < slothdb_row_count(result); r++) {
        printf("%s: %s\n",
            slothdb_column_name(result, 0),
            slothdb_value_varchar(result, r, 0));
    }
    slothdb_free_result(result);

    slothdb_disconnect(conn);
    slothdb_close(db);
}
```

---

## Build from Source

```bash
git clone https://github.com/SouravRoy-ETL/slothdb.git
cd slothdb
cmake -B build -DSLOTHDB_BUILD_SHELL=ON
cmake --build build --config Release

# Run CLI
./build/src/Release/slothdb_shell

# Run tests (325 tests, 131K assertions)
./build/test/Release/slothdb_tests
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `SLOTHDB_BUILD_SHELL` | OFF | Build the CLI shell |
| `SLOTHDB_BUILD_TESTS` | ON | Build test suite |
| `SLOTHDB_CUDA` | OFF | Enable NVIDIA GPU acceleration |
| `SLOTHDB_METAL` | OFF | Enable Apple Silicon GPU acceleration |

---

## Full SQL Reference

130+ SQL features. [See complete reference](docs/SQL_REFERENCE.md).

**Highlights:**
- All standard SQL: SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, ALTER
- Joins: INNER, LEFT, RIGHT, FULL, CROSS
- Aggregates: COUNT, SUM, AVG, MIN, MAX, STDDEV, MEDIAN, STRING_AGG
- Window functions: ROW_NUMBER, RANK, DENSE_RANK, NTILE, LEAD, LAG
- QUALIFY clause (Snowflake-style)
- CTEs: WITH, WITH RECURSIVE
- Set operations: UNION, INTERSECT, EXCEPT
- MERGE (upsert), TRUNCATE, EXPLAIN
- 40+ scalar functions (string, math, date, regex)
- Transactions: BEGIN, COMMIT, ROLLBACK

**File formats (all built-in, no extensions needed):**

| Format | Read | Write | Function |
|--------|------|-------|----------|
| CSV | Yes | Yes | `read_csv()` / `COPY TO` |
| JSON / NDJSON | Yes | Yes | `read_json()` / `COPY TO FORMAT JSON` |
| Parquet | Yes | Yes | `read_parquet()` / `COPY TO FORMAT PARQUET` |
| Arrow IPC | Yes | Yes | `read_arrow()` |
| Avro | Yes | - | `read_avro()` |
| Excel (.xlsx) | Yes | - | `read_xlsx()` |
| SQLite | Yes | - | `sqlite_scan()` |

---

## Architecture

```
SQL String → Parser → Binder → Planner → Optimizer → Executor → Results
                                                         |
                                              GPU Engine (CUDA/Metal)
                                              or CPU (vectorized, parallel)
```

- **Storage:** Columnar with row groups, compression (RLE, dictionary, bitpacking), zone maps
- **Execution:** Vectorized (2048 values/batch), morsel-driven parallelism, GPU offload for >100K rows
- **Persistence:** Single `.slothdb` file, auto-save on close

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). PRs welcome!

## License

MIT

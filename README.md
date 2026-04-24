<div align="center">

<img src="assets/hero.svg" alt="SlothDB" width="100%">

<h3>Run analytics faster.</h3>

<p>SlothDB is an embedded SQL database that runs everywhere: on your laptop, on a server, and in the browser. Built from scratch. <b>Up to 5x faster</b> where it counts.</p>

[![PyPI](https://img.shields.io/pypi/v/slothdb?color=3775A9&logo=pypi&logoColor=white&cacheSeconds=60)](https://pypi.org/project/slothdb/)
[![npm](https://img.shields.io/npm/v/@slothdb/wasm?color=CB3837&logo=npm&label=npm)](https://www.npmjs.com/package/@slothdb/wasm)
[![PyPI downloads](https://static.pepy.tech/badge/slothdb)](https://pepy.tech/project/slothdb)
[![PyPI/month](https://static.pepy.tech/badge/slothdb/month)](https://pepy.tech/project/slothdb)
[![npm downloads](https://img.shields.io/npm/dt/@slothdb/wasm?label=npm%20downloads&color=CB3837)](https://www.npmjs.com/package/@slothdb/wasm)
[![CI](https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml/badge.svg)](https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Stars](https://img.shields.io/github/stars/SouravRoy-ETL/slothdb?style=social)](https://github.com/SouravRoy-ETL/slothdb)

[Website](https://slothdb.org) · [**Playground**](https://slothdb.org/playground/) · [Blog](https://slothdb.org/blog/compiling-a-database-to-wasm.html) · [Docs](docs/DOCUMENTATION.md) · [Benchmarks](#performance) · [Python](docs/DOCUMENTATION.md#6-python-api) · [SQL Guide](docs/DOCUMENTATION.md#4-sql-guide)

<br>

<img src="assets/demo.svg" alt="SlothDB 60-second demo - side-by-side timing vs DuckDB" width="90%">

</div>

---

## Ask in any language. Get SQL.

Type `.ask` at the `slothdb>` prompt. A rules parser handles catalog questions and common English shapes in under 10 ms with no model. Anything else falls through to a local Qwen2.5-Coder (0.5B for simple, 1.5B for analytic; lazy-downloaded on first use under `-DSLOTHDB_ASK_MODEL=ON`), which speaks 29 natural languages: English, Chinese, Spanish, French, German, Japanese, Korean, Russian, Arabic, Portuguese, Italian, Hindi, and more. Every generated statement is shown before it runs. Nothing leaves the machine. Set `SLOTHDB_ASK_CONFIRM=1` to add a `[Y/n]` prompt before each run.

<div align="center">
  <img src="assets/ask-demo.svg" alt=".ask: rules-first, router, two local Qwens, [Y/n] gate" width="100%">
</div>

| tier | what | cost | covers |
|---|---|--:|---|
| 1 | **Rules parser** (default) | sub-10 ms, no model | catalog, COUNT/SUM/AVG/GROUP BY/TOP-N, file-source |
| 2 | **Local Qwen 2.5-Coder 0.5B Q4_K_M** | ~200 ms, ~310 MB | open-ended SELECT/GROUP BY/filter |
| 3 | **Local Qwen 2.5-Coder 1.5B Q4_K_M** | ~500 ms, ~986 MB | window functions, ranking within groups, LAG/LEAD, joins |

Both model tiers download lazily in parallel on first `.ask` (total ~1.3 GB). Router is a pure function of the question: no LLM call involved in routing. Cumulative / running / moving aggregates refuse cleanly (engine gap, not model gap). Full spec, router signals, refusal policy: [docs/ASK.md](docs/ASK.md).

## Try it in 60 seconds

**In your browser** - no install, no account: **[slothdb.org/playground](https://slothdb.org/playground/)**. Full SlothDB compiled to WebAssembly, with a pre-loaded 1,000-row demo CSV + matching Parquet to compare format performance. Files you add stay on your machine.

**In Python** - CPython 3.8+ on Linux / macOS / Windows (see [latest release](https://github.com/SouravRoy-ETL/slothdb/releases/latest) for published wheel tags; falls back to source build if no wheel matches):

```bash
pip install slothdb
python -c "import slothdb; slothdb.demo()"
```

That generates a 100 000-row CSV, runs three queries, and prints the side-by-side with DuckDB shown above. No files to find, no setup.

```python
# Your own files, same API. pandas round-trip in two lines:
import slothdb, pandas as pd
db = slothdb.connect()
df = db.sql("SELECT region, SUM(revenue) AS rev FROM 'sales.parquet' GROUP BY region").fetchdf()
```

**In Node/JS** - `npm install @slothdb/wasm`:

```js
import { SlothDB } from '@slothdb/wasm';
const db = await SlothDB.create();
const { columns, rows } = db.query("SELECT 1 AS n");
```

**In the shell** - [download](https://github.com/SouravRoy-ETL/slothdb/releases/latest) or build from source; then `slothdb analytics.slothdb` for a persistent single-file DB.

---

## What's new in 0.1.7

- **`.ask` in the shell.** Optional natural-language sub-REPL. Rules parser in every build; on-device Qwen fallback under `-DSLOTHDB_ASK_MODEL=ON`. [docs/ASK.md](docs/ASK.md).

- **CREATE TABLE AS SELECT.** `CREATE [OR REPLACE] TABLE t AS SELECT ...` works end-to-end, including from files (`CREATE TABLE sales AS SELECT * FROM 'sales.csv'`).
- **Catalog-introspection C API** - five new functions (`slothdb_table_count`, `_name`, `_column_count`, `_column_name`, `_column_type`) enumerate tables + columns from any binding. `.ask` consumes it; `list_tables()` / `describe_table()` in the Python wheel come next.
- **Benchmark table now includes the 1.04× Parquet SUM tie** instead of hiding it. Headline is 3.9× on the 5-query warm batch (reproducible via the demo).

403 tests, 131 513 assertions, green on Windows / Linux / macOS.

### Previously in 0.1.6

JOIN hot path: 138 ms vs 540 ms on the 5-query warm batch. `CREATE LIVE VIEW` with incremental CSV append. Edge build (`-DSLOTHDB_EDGE=ON`) for sub-MB WASM bundles under Cloudflare Workers' 1 MB cap. `DESCRIBE`, `PRAGMA table_info`, `VARCHAR(n)` enforcement. Full notes in [CHANGELOG.md](CHANGELOG.md).

---

## Why SlothDB?

SlothDB is an **embedded analytical database in C++20**. You link it into your application (or run the shell) and point SQL at files on disk. No server process, no import step, no "load the extension first." That's the same model as DuckDB and SQLite, but the defaults are different.

```sql
-- No CREATE TABLE. No COPY FROM. Just point at the file.
SELECT department, COUNT(*), AVG(salary)
FROM 'employees.parquet'
WHERE hire_year >= 2020
GROUP BY department
ORDER BY AVG(salary) DESC;

-- Local, HTTP(S), or public S3 - same SQL.
SELECT region, SUM(revenue) FROM 'https://host/data.csv' GROUP BY region;
SELECT * FROM 's3://public-bucket/events.parquet';
```

### If you're already using DuckDB

DuckDB is great and a lot of users should keep using it. SlothDB overlaps on the embedded-columnar-SQL story but makes different default choices. Four of those choices are what we've heard asked for most:

- **Live views over growing files.** `CREATE LIVE VIEW` caches a query result and, for the common `SELECT * FROM 'file.csv'` shape on CSV/TSV, appends only the new bytes on the next query. Useful when a dashboard tails a log that keeps growing.
- **Smaller WASM for edge workers.** The `-DSLOTHDB_EDGE=ON` build (CSV/JSON/Parquet only) targets Cloudflare Workers' 1 MB script budget; the full WASM bundle is around 1.3 MB.
- **Everything in core, no extensions.** HTTP(S), S3 (anonymous public reads), Avro, Excel, Arrow, and SQLite read through the same core binary - no separate install/load step.
- **Stable C ABI + numeric error codes.** `ErrorCode::TABLE_NOT_FOUND = 2000` does not shift between releases; bindings built against 0.1.x keep working.

Same idea as DuckDB otherwise: embedded, columnar, vectorized, query files directly. Head-to-head on our bench:

| | SlothDB | DuckDB |
|---|---|---|
| 5-query warm JOIN batch (1 M × 1 K) | **138 ms** | 540 ms |
| Live-refresh view on a growing CSV | `CREATE LIVE VIEW`, incremental append | re-execute the query |
| File-format readers in the core binary | 7 (CSV, Parquet, JSON, Avro, Excel, Arrow, SQLite) | 3 in core; Avro/Excel/SQLite via extensions |
| Remote file read from SQL | built in (HTTP(S), public S3) | `httpfs` extension |
| WASM bundle size | 1.3 MB full / sub-1 MB edge | ~18 MB |
| Extension ABI | stable C ABI, numeric error codes | internal C++ API |
| `VARCHAR(n)` length | enforced at INSERT | not enforced |
| Binary size (CLI) | ~1-2 MB (Windows MSVC Release; Linux static ~2-4 MB) | ~50 MB (latest release tarball) |
| License | MIT | MIT |

Numbers are from our machine; reproduce with the demo below. The big gaps are architectural: native Avro decoder, built-in `httpfs`-equivalent, smaller WASM.

### If you're using ClickHouse today

ClickHouse wins at petabyte-scale distributed analytics - SlothDB isn't trying to replace it there. But if your workload fits on one machine (Python notebooks, desktop analytics, embedded BI, single-node dashboards), you're paying ClickHouse-server operational cost for work that doesn't need a cluster:

| | SlothDB | clickhouse-local | ClickHouse server |
|---|---|---|---|
| Deployment | 1-2 MB binary, embedded | ~500 MB binary | server + Keeper + config |
| Cold start | < 10 ms on our bench | seconds (varies) | tens of seconds (varies) |
| Ops overhead | none | none | daemon, ports, upgrades |
| Embed in a desktop app | yes, one binary | awkward | no |
| Cluster / distributed query | no | no | yes |

For single-node SQL over local files, a ClickHouse server is operational overkill; `clickhouse-local` is closer in spirit but still a ~500 MB binary with slower cold start. SlothDB targets this narrow slice - single-node, embedded, file-first.

### If you're using SQLite today for analytics

SQLite is row-oriented and tuned for transactional workloads. Aggregate queries over wide tables read every column of every row even when you only need two. SlothDB is columnar and vectorized, so column-selective aggregates are typically several× faster - the exact speedup depends on row count and column width, so reproduce with your own data. You can keep your existing SQLite file and read from it directly with `sqlite_scan('app.db', 'users')`.

### What SlothDB does not do (honest list)

- **No distributed query execution.** One-node embedded engine. Use ClickHouse if you outgrow one machine.
- **No MVCC / multi-writer transactions.** Single writer, crash-safe checkpoint. OLTP workloads are a poor fit.
- **No secondary indexes yet.** Scan-based execution. Zone-map pruning helps on sorted data, but there's no B-tree / hash index for point lookups.
- **Window-function coverage is partial.** Plain OVER / PARTITION BY works; `ROWS BETWEEN ...` frames and `SUM OVER (ORDER BY)` cumulative shapes have known gaps (tracked in [docs/ROADMAP.md](docs/ROADMAP.md)).
- **Authenticated S3 and TLS cert pinning are not implemented.** `s3://` works for anonymous public-bucket reads only.
- **UPDATE / DELETE are not parallelized.** They rewrite affected chunks serially - fine for thousands of rows, slow for millions.
- **Young codebase.** 403 tests, five benchmark formats green, but corners of SQL will still surprise you. File an issue with a repro and we'll fix it.

---

## Quickstart

**60-second tour** (no files to find - it generates and queries synthetic data, and prints a side-by-side with DuckDB if you have it installed):

```bash
pip install slothdb
python -c "import slothdb; slothdb.demo()"
```

```
Query                            SlothDB     DuckDB    Speedup
--------------------------------------------------------------
COUNT(*)                          3.1 ms    17.0 ms     5.48x
SUM(revenue) WHERE year>=2023    10.6 ms    17.7 ms     1.67x
GROUP BY region                  10.0 ms    19.1 ms     1.91x
```

**Query your own files** - one-shot or interactive shell:

```bash
pip install slothdb                                                              # Python
curl -fsSL https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/install.sh | bash   # Linux / macOS CLI
# Windows: download slothdb.exe from https://github.com/SouravRoy-ETL/slothdb/releases/latest
```

```bash
slothdb -c "SELECT region, SUM(revenue) FROM 'sales.csv' GROUP BY region ORDER BY 2 DESC;"
slothdb                              # interactive, in-memory
slothdb analytics.slothdb            # interactive, persistent
```

```python
import slothdb
db = slothdb.connect()
df = db.sql("SELECT * FROM 'employees.csv' WHERE salary > 100000").fetchdf()
```

<details>
<summary><b>More install methods (Debian, Fedora, Arch, Homebrew, build from source)</b></summary>

| Platform | Command |
|----------|---------|
| Ubuntu / Debian | `sudo dpkg -i slothdb_0.1.7_amd64.deb` ([download](https://github.com/SouravRoy-ETL/slothdb/releases/latest)) |
| Fedora / RHEL | `sudo rpm -i slothdb-0.1.7.rpm` (build from [spec](packaging/rpm/slothdb.spec)) |
| Arch Linux | `makepkg -si` ([PKGBUILD](packaging/arch/PKGBUILD)) |
| macOS (Homebrew) | `brew install --build-from-source packaging/homebrew/slothdb.rb` |
| Build from source | See [below](#build-from-source) |

</details>

## Performance

> 1 M-row datasets · warm cache · 5-run median on one workstation · DuckDB 1.1.3 · Ryzen 7 5800X, 32 GB DDR4, Windows 11, MSVC Release. Numbers will differ on other hardware - reproduce with `pip install slothdb && python -c "import slothdb; slothdb.demo()"`, which runs the 5-query batch side-by-side with whatever DuckDB version is installed.

### 1. JOIN - CPU-bound, new in 0.1.6

```
SELECT COUNT(*) FROM big JOIN sm ON b.k = s.k   -- 1 M × 1 K

SlothDB  85 ms        DuckDB  212 ms        2.5× faster
```

Pure hash-join hot path, no I/O ambiguity. Typed int64 hash path, parallel CSV pre-parse, build-side projection pushdown, `COUNT(*)`-over-JOIN fused into the aggregate. Landed in this release.

### 2. End-to-end batch - five queries in one shell invocation

```
scan + aggregate + GROUP BY + filter + JOIN

SlothDB total  138 ms        DuckDB total  540 ms        3.9× faster
```

Mixed workload. Startup cost is part of the denominator - that's honest: it's what someone running `slothdb -c "..."` actually pays. Not a microbench.

### 3. Avro - native decode beats an extension path

```
SUM(revenue) on 1 M-row .avro           SlothDB  140 ms   DuckDB  760 ms   5.43×
GROUP BY region on 1 M-row .avro        SlothDB  170 ms   DuckDB  800 ms   4.71×
```

Native typed Avro decoder in the core binary vs. an extension-based reader. Architectural difference, not a micro-optimization.

<details>
<summary><b>Full 16-query suite across CSV / Parquet / JSON / Avro / Excel</b></summary>

| Format | Query | SlothDB | DuckDB | Speedup |
|---|---|--:|--:|:-:|
| CSV | `COUNT(*)` (parser throughput) | 33 ms | 170 ms | 5.08× |
| CSV | `SUM(revenue)` | 106 ms | 177 ms | 1.67× |
| CSV | `GROUP BY region` | 100 ms | 191 ms | 1.91× |
| CSV | `GROUP BY product, year` | 117 ms | 198 ms | 1.70× |
| CSV | `WHERE year>=2023 AND qty>100 GROUP BY region` | 107 ms | 194 ms | 1.81× |
| CSV | `big × small JOIN COUNT(*)` (1 M × 1 K) | 85 ms | 212 ms | 2.49× |
| Parquet | `COUNT(*)` | 12 ms | 34 ms | 2.83× |
| Parquet | `SUM(revenue)` | 46 ms | 48 ms | 1.04× (tie, within noise) |
| Parquet | `GROUP BY region` | 76 ms | 88 ms | 1.16× |
| Parquet | `GROUP BY product, year` | 146 ms | 173 ms | 1.18× |
| Parquet | `WHERE + GROUP BY` | 157 ms | 198 ms | 1.26× |
| JSON | `SUM(revenue)` | 242 ms | 314 ms | 1.30× |
| JSON | `GROUP BY region` | 284 ms | 324 ms | 1.14× |
| Avro | `SUM(revenue)` | 140 ms | 760 ms | 5.43× |
| Avro | `GROUP BY region` | 170 ms | 800 ms | 4.71× |
| Excel | `GROUP BY region` | 2500 ms | 3560 ms | 1.41× |

Median speedup: 1.70×. Range: 1.04× - 5.43×.

</details>

Caveats worth knowing: Parquet aggregates are within ~20 % of DuckDB on most queries - both engines saturate the columnar fast path there, so don't expect 3× on Parquet. The big gaps come from SlothDB's native decoders (Avro, CSV `COUNT(*)`) and the 0.1.6 JOIN hot path. We have not submitted to ClickBench yet - on the roadmap.

The architectural decisions behind the numbers (typed columnar decode, per-worker buffer reuse, fused scan+aggregate, zero-copy VARCHAR, vectorized filter, parallel CSV aggregate, typed int64 JOIN hash path) are in [CHANGELOG.md](CHANGELOG.md) with a commit per optimization.

## Query Any File with SQL

No import step. No schema definition. Just query:

```sql
-- CSV
SELECT * FROM 'sales.csv';
SELECT region, SUM(revenue) FROM read_csv('data/*.csv') GROUP BY region;

-- Parquet (fastest - columnar, compressed, filter pushdown)
SELECT * FROM read_parquet('events.parquet') WHERE event_date > '2024-01-01';

-- JSON / NDJSON
SELECT status, COUNT(*) FROM 'api_logs.json' GROUP BY status;

-- Excel
SELECT * FROM read_xlsx('quarterly_report.xlsx');

-- Avro, Arrow IPC, SQLite - all built-in, no extensions
SELECT * FROM read_avro('events.avro');
SELECT * FROM sqlite_scan('app.db', 'users');
```

**Create views on files - always returns fresh data:**

```sql
CREATE VIEW sales AS SELECT * FROM read_csv('sales.csv');
CREATE VIEW events AS SELECT * FROM read_parquet('events.parquet');
CREATE VIEW report AS SELECT * FROM read_xlsx('report.xlsx');

-- Query views like tables - re-reads the file each time
SELECT region, SUM(revenue) FROM sales GROUP BY region;
```

**Export results to any format:**

```sql
COPY (SELECT * FROM 'big.csv' WHERE year >= 2024) TO 'filtered.parquet' WITH (FORMAT PARQUET);
```

> **[Full file format guide](docs/DOCUMENTATION.md#2-query-your-files)** - CSV, Parquet, JSON, Excel, Arrow, Avro, SQLite, virtual views

## Persistent Database

```bash
slothdb analytics.slothdb    # creates or opens a .slothdb file
```

```sql
CREATE TABLE sales AS SELECT * FROM read_csv('sales_2024.csv');
CREATE TABLE events AS SELECT * FROM read_parquet('events.parquet');

-- Next session, tables are still here
SELECT region, SUM(revenue) FROM sales GROUP BY region;
```

> **[Working with large datasets](docs/DOCUMENTATION.md#3-working-with-large-datasets)** - when to query directly vs. import vs. convert to Parquet

## Python

```python
import slothdb

db = slothdb.connect()                    # in-memory
db = slothdb.connect("analytics.slothdb") # persistent

# Query files directly
result = db.sql("SELECT * FROM 'employees.csv' WHERE salary > 100000")
df = result.fetchdf()  # pandas DataFrame

# Window functions, CTEs, QUALIFY - full SQL
result = db.sql("""
    SELECT name, department, salary
    FROM 'employees.parquet'
    QUALIFY ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) = 1
""")
```

> **[Full Python API reference](docs/DOCUMENTATION.md#6-python-api)** - connect, query, results, pandas integration, context manager

## C/C++

```c
#include "slothdb/api/slothdb.h"

slothdb_database *db;
slothdb_connection *conn;
slothdb_result *result;

slothdb_open("analytics.slothdb", &db);
slothdb_connect(db, &conn);
slothdb_query(conn, "SELECT region, SUM(revenue) FROM read_csv('sales.csv') GROUP BY region", &result);

for (uint64_t r = 0; r < slothdb_row_count(result); r++)
    printf("%s: %s\n", slothdb_value_varchar(result, r, 0), slothdb_value_varchar(result, r, 1));

slothdb_free_result(result);
slothdb_disconnect(conn);
slothdb_close(db);
```

> **[Full C/C++ API reference](docs/DOCUMENTATION.md#7-cc-api)** - lifecycle, queries, results, error handling, CMake integration, RAII wrapper

## Features

| Category | Details |
|----------|---------|
| **SQL** | 130+ features - JOINs, CTEs (recursive), window functions, QUALIFY, MERGE, subqueries, set operations |
| **Live file views** | `CREATE LIVE VIEW` caches the result and auto-refreshes on file change. Incremental CSV append path parses only new bytes on log-tail workloads |
| **Shell `.ask`** | Natural-language -> SQL in the CLI. Rules parser (default, ~50 KB, no network) handles COUNT / SUM / AVG / GROUP BY / TOP-N / year filters / load-file / count-rows-in-file / create-view-from-file. Builds with `-DSLOTHDB_ASK_MODEL=ON` fall back to a local Qwen2.5-Coder GGUF for open-ended NL. Every generated SQL is shown before it runs; set `SLOTHDB_ASK_CONFIRM=1` to require a `[Y/n]` keypress per statement. |
| **Metadata** | `DESCRIBE <query>`, `DESCRIBE <table>`, `PRAGMA table_info('t')`, `PRAGMA database_list` - BI-tool introspection out of the box |
| **Type constraints** | `VARCHAR(n)` length enforced on INSERT (no silent truncation) |
| **File I/O** | CSV, Parquet, JSON, Arrow, Avro, Excel, SQLite - all built-in with auto-detection, glob patterns, virtual views |
| **Remote files** | `https://` and public-bucket `s3://` URLs work directly in any SQL path |
| **Functions** | 70+ functions - string, math, date/time (including `DATE_TRUNC` with WEEK/QUARTER/DECADE + `MONTHNAME` / `DAYNAME` / `LAST_DAY` / `MAKE_DATE`), aggregate, regex, trigonometric |
| **Performance** | Vectorized columnar engine (2,048 values/batch), morsel-driven parallelism, fused scan+aggregate, typed int64 JOIN hash path, parallel CSV pre-parse, zero-copy VARCHAR |
| **Build flavours** | Default full build (~1-4 MB native binary depending on platform and linking) or `-DSLOTHDB_EDGE=ON` for sub-MB WASM bundles that fit under Cloudflare Workers' 1 MB cap |
| **Storage** | Single-file `.slothdb` persistence, RLE/dictionary/bitpacking compression, zone maps |
| **Optimizer** | Constant folding, filter pushdown, TopN optimization |
| **APIs** | CLI shell, Python (with pandas), C/C++ (stable ABI) |
| **Reliability** | 403 tests, 131,513 assertions, bounds-checked parsing, DoS limits |

## Documentation

| | |
|-|-|
| **[Full Documentation](docs/DOCUMENTATION.md)** | Complete guide - install, file queries, SQL, Python, C/C++, extensions |
| [Query Your Files](docs/DOCUMENTATION.md#2-query-your-files) | CSV, Parquet, JSON, Excel, Arrow, Avro, SQLite |
| [Large Datasets](docs/DOCUMENTATION.md#3-working-with-large-datasets) | Import strategies, Parquet conversion, persistence |
| [SQL Guide](docs/DOCUMENTATION.md#4-sql-guide) | Joins, window functions, CTEs, QUALIFY, MERGE |
| [All Functions](docs/DOCUMENTATION.md#5-all-functions) | 70+ built-in functions with examples |
| [Python API](docs/DOCUMENTATION.md#6-python-api) | Connect, query, pandas, context manager |
| [C/C++ API](docs/DOCUMENTATION.md#7-cc-api) | Lifecycle, queries, results, CMake, RAII |
| [SQL Quick Reference](docs/SQL_REFERENCE.md) | One-page cheat sheet |
| [Extension API](include/slothdb/extension/extension_api.h) | Build custom extensions |

## Build from Source

```bash
git clone https://github.com/SouravRoy-ETL/slothdb.git
cd slothdb
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/src/slothdb          # Linux/macOS
build\src\Release\slothdb.exe  # Windows
```

**Run tests:**

```bash
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DSLOTHDB_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release    # 403 tests
```

| Build Option | Description |
|-------------|-------------|
| `-DSLOTHDB_BUILD_SHELL=ON` | Build CLI shell |
| `-DSLOTHDB_BUILD_TESTS=ON` | Build test suite |
| `-DSLOTHDB_SANITIZERS=ON` | Enable ASan/UBSan |
| `-DSLOTHDB_EDGE=ON` | Edge / WASM minimal build - strips Excel / Avro / Arrow IPC / SQLite readers. Target: sub-1 MB WASM for Cloudflare Workers. See [docs/EDGE_BUILD.md](docs/EDGE_BUILD.md) |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions and contribution guidelines.

## License

[MIT](LICENSE) - use it however you want.

---

<div align="center">

<sub>Built with C++20 · Zero dependencies · <a href="https://github.com/SouravRoy-ETL">@SouravRoy-ETL</a></sub>

</div>

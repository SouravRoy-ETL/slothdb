# Changelog

All notable changes to SlothDB are documented here.

## 0.1.6 — JOIN perf overhaul, ORDER BY correctness, DuckDB-parity metadata

The JOIN hot path goes from ~1.3× slower than DuckDB to ~2.5× faster on the big×small join benchmark, two latent ORDER BY / aggregate correctness bugs get closed, and three roadmap-driven metadata features land: DESCRIBE, PRAGMA, and VARCHAR(n) enforcement. **373 tests / 131 446 assertions** green on Windows, Linux, macOS.

### JOIN: from slower than DuckDB to faster

The 1 M-row big × 1 K-row small join benchmark (`SELECT COUNT(*) FROM big JOIN sm ON b.k = s.k`) went from **288 ms → 85 ms** on SlothDB while DuckDB stays at ~212 ms — a 3.4× improvement that flipped the scoreboard from 1.3× slower to 2.5× faster. Five commits stacked:

- **Typed int64 hash path.** Build-side and probe-side integer keys go through `std::unordered_map<int64_t, …>` — no `Value::ToString()` per lookup, no `std::string` allocation per probe row.
- **Build-side projection pushdown.** The join now tells its child scan which columns actually feed downstream consumers before materialising; wide build tables no longer parse every column.
- **SIMD unquoted-field scan in FastCSVReader.** `NextField` uses SSE2 three-way `find` to skip to the next delimiter / newline / quote, 16 bytes at a time. Tight loop, no branch mis-prediction.
- **Parallel CSV pre-parse for ≥ 2 MB files.** `PhysicalFileScan::GetData` decides once (via `use_parallel_`) whether to stream chunks serially or hand the whole file to `FastCSVReader::ReadIntoChunks`, which splits line-aligned byte ranges across the available hardware threads.
- **File-size-based build-side selection.** When both children are `PhysicalFileScan`, the join sides the build table by comparing `std::filesystem::file_size` instead of materialising both sides and counting rows. Avoids double-parse on big × small.

### JOIN: reverse-order equi-joins returned 0 rows

`FROM sm s JOIN big b ON b.k = s.k` matched nothing while the identical-semantics `ON s.k = b.k` returned 1 M rows. The join classified `cmp.left` as the LEFT-table side unconditionally, but a user-written condition can put the RIGHT-table column first. Fixed by classifying each operand by its combined column index (`< left_col_count_` → LEFT; ≥ → RIGHT) and assigning build / probe columns accordingly. Pre-existing bug uncovered during perf benchmarking.

### JOIN + aggregate fusion: COUNT(*) and int64 keys

`TryComputeFusedJoinAggregate` skipped queries with no GROUP BY (like `SELECT COUNT(*) FROM A JOIN B …`) and silently only wired the string-keyed hash table. Two fixes:

1. Drop the no-group guard and pre-seed a single accumulator so `COUNT = 0` on empty joins is preserved.
2. Add an int64 lookup branch in both the parallel and sequential fused probes, mirroring the typed probe path `PhysicalHashJoin` already uses.

The q3 benchmark jump above is almost entirely this change — without it, the plan falls back to `PhysicalHashJoin::GetData`, which does one `Value`-boxed round-trip per probe-row column across 1 M matches.

### Planner: aggregate output projected back to SELECT-list order

`PhysicalHashAggregate` emits rows as `[groups…, aggregates…]` internally regardless of how the user wrote the SELECT list, so `SELECT COUNT(*), k FROM t GROUP BY k` came back as `[k, count]`. Fixed by building a `LogicalProjection` on top of every `LogicalAggregate` that picks columns out of the aggregate's internal schema in user-SELECT order. For `GROUP BY k`, the projection emits col 0 = count, col 1 = k as the user wrote. Pre-existing; also surfaced as a `GetValue for type INVALID` error in some shapes before the reorder.

### Planner: ORDER BY below the plain projection

`SELECT label FROM t ORDER BY label LIMIT 3` segfaulted when the projection narrowed the schema: the ORDER BY's `column_index` stayed in source-schema (from the binder) while `PhysicalOrderBy` read from projection-output rows. When the projected width was less than the bound `col_idx`, access went out of bounds on a release build. The symptoms varied — crash on `SELECT label ORDER BY label`, silent mis-sort on `SELECT label, k ORDER BY label` (sorts by k at index 1).

Fixed by deferring the plain projection until after ORDER BY in the planner. `PhysicalDistinct` preserves first-occurrence order, so DISTINCT after the deferred projection still yields a sorted final output. Aggregate / window placement is unchanged — their ORDER BY has separate, lower-priority issues (tracked but not this release).

### DESCRIBE: `DESCRIBE <query>` and `DESCRIBE <table>`

Roadmap #1 for 0.1.6. DuckDB's first-query-on-a-new-file move now works in SlothDB: bind the inner SELECT, emit its `result_names` / `result_types` as six columns (`column_name, column_type, null, key, default, extra`). Output matches DuckDB byte-for-byte. `DESCRIBE <table>` desugars to `DESCRIBE SELECT * FROM <table>` so bare table names work the same way.

```sql
DESCRIBE SELECT * FROM employees;
DESCRIBE employees;
DESCRIBE SELECT region, SUM(revenue) AS total FROM sales GROUP BY region;
```

### PRAGMA introspection: `table_info` and `database_list`

Roadmap #2. DBT, Metabase, SQLAlchemy, DBeaver all introspect via PRAGMAs through their JDBC/ODBC drivers — without these, SlothDB can't plug into a BI tool. New `PRAGMA` keyword, `PragmaStatement` AST, and dispatch in `Connection::Query`:

- `PRAGMA table_info('t')` → `(cid, name, type, notnull, dflt_value, pk)` rows per column, DuckDB-format. `notnull` / `pk` hardcoded to false because the catalog does not yet track them.
- `PRAGMA database_list` → single in-memory database until `ATTACH` lands.
- `pragma_table_info` and `pragma_database_list` aliases are also accepted.

### VARCHAR(n): length enforcement on INSERT

Roadmap #4. Parser already read the `(n)`; the binder was throwing it away. Now `VarcharTypeInfo` carries the max length, and `PhysicalInsert` rejects rows whose string value exceeds it — no more silent truncation where you discover a VARCHAR(5) silently held 200 chars in production. Stricter than DuckDB, which ignores the length.

```sql
CREATE TABLE t (code VARCHAR(3));
INSERT INTO t VALUES ('abc');    -- OK
INSERT INTO t VALUES ('toolong'); -- Error: Value too long for column 0 (VARCHAR(3): got 7 chars)
```

Bare `VARCHAR` / `TEXT` / `STRING` stay unbounded, so existing tables behave identically. `DESCRIBE` and `PRAGMA table_info` both render the declared length (e.g. `VARCHAR(3)`).

### Platform

- CMake project version bumped to 0.1.6. Engine `slothdb_version()` returns `"0.1.6"`.
- Test suite: 363 → 373 (10 new tests covering DESCRIBE, PRAGMA, VARCHAR(n)).

---

## 0.1.5 — WASM playground, npm package, engine fixes

Browser-runnable build plus a wave of binder/planner fixes shaken out by the playground and community feedback. **363 tests / 131 408 assertions** green on Windows, Linux, macOS.

### New distribution surfaces

- **WebAssembly build** — full engine compiles to a 1.3 MB `slothdb.wasm` + 97 KB JS glue via Emscripten. Live playground at **https://slothdb.org/playground** with drag-drop CSV / Parquet / JSON / Arrow / SQLite / Excel upload, CodeMirror SQL editor, sortable result grid, and CSV export.
- **npm package** — `npm install @slothdb/wasm` ships the same WASM build with a clean JS wrapper and TypeScript types. Published at https://www.npmjs.com/package/@slothdb/wasm.
- **Technical blog post** — `/blog/compiling-a-database-to-wasm.html` walks through the threading / exceptions / HTTP / CodeMirror bundling gotchas.

### Parser: non-reserved keywords as identifiers (DuckDB parity)

`MONTH`, `DAY`, `YEAR`, `HOUR`, `ROWS`, `COUNT`, `SUM`, all type names, all constraint keywords, MERGE/PIVOT/transaction verbs — any keyword that isn't a clause-starter or operator is now legal as a column alias, table alias, or column name. Previously `SELECT COUNT(*) AS rows FROM t` threw `Expected identifier after AS`. Now it works. Regression tests cover negative cases (AS where / AS join still throw) so the reserved set stays tight.

### Binder: `SELECT * ... ORDER BY <named_column>`

The ORDER-BY alias-resolution fast-path re-bound `select_list[i]` when the sort column matched a result name. With `SELECT *`, every expanded name mapped back to `select_list[0]` which was an unbound STAR expression — re-binding it as a scalar threw `Internal Error: Unhandled expression type in binder`. Fixed by skipping the alias path for star entries and falling through to normal column-name resolution.

### Connection: UNION right side + JOIN right side preprocessing

The file-literal preprocessing (view expansion, `read_csv` / `read_parquet` / `read_json` / `read_arrow` / `read_avro` / `read_xlsx` / `sqlite_scan` / `__FILE__` auto-detect) only walked the left-hand root of the FROM clause. Two fixes:

1. **UNION / INTERSECT / EXCEPT**: wrapped preprocessing in a loop that walks the `set_right` chain. Without it, `SELECT ... FROM 'a.csv' UNION ALL SELECT ... FROM 'b.parquet'` failed with `Table '__FILE__' not found`.
2. **JOIN chain**: same preprocessing now walks `from_table->right` recursively (deepest-first detach / swap / reattach). Without it, `FROM 'a.csv' JOIN 'b.csv' ON ...` failed identically.

### Connection: RAII cleanup for temp tables

A failing query used to leave zombie `__auto_file__` / `__read_parquet__` / CTE temp tables in the catalog because the cleanup block only ran on the success path. Replaced with a stack-allocated `TableCleanupGuard` declared at the top of each statement iteration — drops on scope exit, whether the query succeeded or threw. Prevents the `Table '__auto_file__' already exists` error that bit users on the second query after an error.

Also: per-iteration unique suffix on every auto-generated temp-table name (`__auto_file_N__`, `__read_parquet_N__`, etc.) so the two sides of a UNION with file literals don't overwrite each other's catalog entry.

### Connection: COPY with bare file-literal source (CSV)

`COPY (SELECT ... FROM 'data.csv' WHERE ...) TO 'out.csv'` now works — previously COPY's own preprocessing only recognised `read_csv()` form, so bare `__FILE__` references inside the subquery crashed the binder. Parquet / JSON COPY-from remains a known follow-up.

### Shell: `.open` on a missing/invalid file no longer exits the REPL

`tools/shell/shell.cpp` previously closed the current database *before* attempting the new `.open`. If the new open failed, there were no valid handles left, and the handler did `return 1` — exiting the entire shell process instead of falling back to the `slothdb>` prompt.

Fixed by reordering: open the new database first, swap in only on success. On failure, the old session stays active and the shell returns to the prompt. Also added a hint for the common confusion — when the failing path ends in `.csv` / `.parquet` / `.json` / `.arrow` / `.xlsx` / `.tsv`, the error now suggests `SELECT * FROM '<path>';` which is what the user almost certainly wanted.

### Platform

- CMake project version bumped to 0.1.5. Engine `slothdb_version()` returns `"0.1.5"`.
- Python wheel targets 0.1.6 for the next PyPI release.
- npm package targets 0.1.7 for the next `@slothdb/wasm` publish (0.1.6 shipped the ORDER-BY fix; 0.1.7 adds JOIN + COPY + pre-reserved-keyword fixes).

---

## 0.1.4 — Remote file reading and extended date functions

Two feature additions that close visible DuckDB-parity gaps. 359 tests / 131 382 assertions green on Windows, Linux, macOS.

### Remote file reading — `https://` and `s3://` URLs work directly from SQL

Any table reference that takes a file path now accepts an HTTP(S) or S3 URL:

```sql
SELECT region, SUM(revenue)
FROM 'https://example.com/data.csv'
GROUP BY region;

SELECT * FROM 's3://public-bucket/events.parquet';
```

- Works for all seven built-in formats (CSV, Parquet, JSON, Avro, Arrow, SQLite, Excel). Integration is at the path-resolution layer in `src/main/connection.cpp`, so every existing reader picks it up for free.
- `s3://bucket/key` is rewritten to virtual-host HTTPS (`https://bucket.s3.amazonaws.com/key`) and read anonymously. Public buckets only — AWS SigV4 for private buckets is follow-up work.
- Windows uses WinHTTP for HTTPS; POSIX currently supports HTTP only (POSIX HTTPS will land with the next patch via libcurl).
- No new build dependencies. Reuses the existing `HTTPClient` stack in `src/storage/http_client.cpp`.
- Limitation: the whole URL is downloaded to a temp file before the reader opens it. Fast for small files, wasteful for large Parquet. Range-request streaming into `ParquetReader` is a separate patch.

### Extended `DATE_TRUNC` + four new date scalars

`DATE_TRUNC` previously accepted only `YEAR`, `MONTH`, `DAY`, `HOUR`. Calls like `DATE_TRUNC('week', ts)` or `DATE_TRUNC('quarter', ts)` silently returned the input unchanged — a trap for analytics users. The full DuckDB-compatible interval set is now implemented:

- `MICROSECOND`, `MILLISECOND`, `SECOND`, `MINUTE`, `HOUR`, `DAY`
- `WEEK` (snaps to Monday, ISO 8601)
- `MONTH`, `QUARTER`, `YEAR`
- `DECADE`, `CENTURY`, `MILLENNIUM`

Singular and plural forms both accepted.

Four new scalars matching DuckDB output (validated side-by-side on the real-life-testing demo script):

| Function | Type | Result |
|---|---|---|
| `MONTHNAME(ts)` | `VARCHAR` | `"January"`..`"December"` |
| `DAYNAME(ts)` | `VARCHAR` | `"Sunday"`..`"Saturday"` |
| `LAST_DAY(ts)` | `BIGINT` | Last day of month at 00:00 UTC (leap-aware) |
| `MAKE_DATE(y, m, d)` | `INTEGER` | `YYYYMMDD`, matches `CURRENT_DATE` encoding |

Implementation: `src/execution/expression_executor.cpp` gained 150 LOC in the DATE_TRUNC branch and the four new function branches; `src/binder/binder.cpp` gets the return-type registrations. Zero subsystem changes — no new types, no parser tokens.

### Testing

- **22 new unit tests** in `test/unit/execution/test_date_functions.cpp` — covers leap-year February, week-start on Sunday/Monday/Friday, decade/century snapping, MAKE_DATE with single-digit and end-of-year days.
- **4 new unit tests** in `test/unit/execution/test_httpfs.cpp` — gated on `SLOTHDB_HTTPFS_ONLINE=1` so offline CI skips cleanly. Fetches `raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/examples/employees.csv` (committed fixture, 12 rows) and verifies COUNT, GROUP BY, WHERE, and ORDER BY.
- **Real-life-testing demo scripts** at `real-life-testing/bench_date_functions.py` and `bench_httpfs.py` reproduce the comparison against `duckdb.exe`.

### Versioning

Shell version string returns `"0.1.4"`, Python wheel bumped to 0.1.4, CMake project version bumped.

## 0.1.3 — Arrow + SQLite on the fast path

Arrow IPC and SQLite were the last two readers still using the bulk-load-to-DataTable roundtrip. Both now stream typed `DataChunk`s at execution time — the same pattern Parquet / JSON / Avro / CSV use.

- **`PhysicalArrowScan`** — `ArrowIPCReader::DetectSchemaLight()` + `ReadIntoChunks()` stream rows directly into `int32_t[]` / `int64_t[]` / `double[]` / `string_t[]` arrays. No Value-boxed intermediate.
- **`PhysicalSQLiteScan`** — `SQLiteScanner::ScanTableIntoChunks()` wraps the existing B-tree scanner but pushes results directly into typed Vectors instead of through `BulkLoadRows`.
- **`TableCatalogEntry::SetSQLitePath(path, table_name)`** + `GetFileSubname()` — needed because SQLite requires both pieces (DB path + target table).
- **Tests:** `test/unit/storage/test_arrow.cpp` (4 cases), `test/unit/storage/test_sqlite.cpp` (3 cases, backed by a committed fixture at `test/fixtures/simple.sqlite`). 333/333 passing.

Shell version string returns `"0.1.3"`, Python wheel bumped to 0.1.3, CMake project version bumped.

Known follow-up: `ORDER BY` on VARCHAR output of `PhysicalSQLiteScan` currently segfaults inside the sort operator. Data reads correctly (SELECT / WHERE / GROUP BY all work); only the specific sort path is affected. Tracked separately.

## 0.1.2

### Performance — SlothDB now beats DuckDB 1.1×–8.6× on every benchmarked format

1 M-row benchmarks, 5-run median, same machine, warm cache:

| Format | Query | SlothDB | DuckDB | Speedup |
|---|---|--:|--:|:-:|
| CSV | `COUNT(*)` | **33 ms** | 170 ms | **5.08× faster** |
| CSV | `SUM(revenue)` | **106 ms** | 177 ms | **1.67× faster** |
| CSV | `GROUP BY region` | **100 ms** | 191 ms | **1.91× faster** |
| CSV | `GROUP BY product, year` | **117 ms** | 198 ms | **1.70× faster** |
| CSV | `WHERE year>=2023 AND qty>100 GROUP BY region` | **107 ms** | 194 ms | **1.81× faster** |
| Parquet | `COUNT(*)` | **12 ms** | 34 ms | **2.83× faster** |
| Parquet | `SUM(revenue)` | **46 ms** | 48 ms | **1.04× faster** |
| Parquet | `GROUP BY region` | **76 ms** | 88 ms | **1.16× faster** |
| Parquet | `GROUP BY product, year` | **146 ms** | 173 ms | **1.18× faster** |
| Parquet | `WHERE year>=2023 AND qty>100 GROUP BY region` | **157 ms** | 198 ms | **1.26× faster** |
| JSON | `SUM(revenue)` | **242 ms** | 314 ms | **1.30× faster** |
| JSON | `GROUP BY region` | **284 ms** | 324 ms | **1.14× faster** |
| Avro | `SUM(revenue)` | **140 ms** | 760 ms | **5.43× faster** |
| Avro | `GROUP BY region` | **170 ms** | 800 ms | **4.71× faster** |
| Excel | `GROUP BY region` (1 M rows) | **2.5 s** | 3.56 s | **1.41× faster** |

Unit tests: 325 / 326 (one pre-existing `CASE + aggregation` failure, unchanged).

The work splits roughly across four layers, applied per format:

### Parquet

Starting point: Parquet `COUNT(*)` tied DuckDB via the footer fast path; every other query was 8–58× slower.

- **Typed columnar decode** (`ParquetColumnData`, `ParquetReader::ReadColumnInto`) — decodes PLAIN / PLAIN_DICTIONARY / RLE_DICTIONARY directly into `i32_data` / `i64_data` / `f64_data` / `str_data` + `str_heap` arrays. Eliminates 10 M `Value` allocations per 10 M-row column that the old row-wise decoder paid.
- **Thread-parallel row-group decode** (`PhysicalParquetScan::WorkerLoop` in slot mode) — worker threads pull RGs via an atomic counter, decode concurrently, deposit results into `slots_[rg]`. Main thread drains in file order via condvar.
- **mmap** (`ParquetReader` constructor) — `CreateFileMapping` + `MapViewOfFile` / `mmap`, replaces all `std::ifstream` per-column opens with pointer arithmetic on the mapped region.
- **Fused scan + aggregate** (`PhysicalParquetScan::PullNextRG` + consumer-mode `RunParallelRGs` + `SetRGConsumer`) — for simple `SUM/COUNT/AVG/MIN/MAX` with no GROUP BY, the aggregate iterates `ParquetColumnData::f64_data[]` directly without materializing a `DataChunk`. Dropped `SUM(revenue)` from 529 ms to 51 ms.
- **Fused GROUP BY with dict-index fast path** (`ParquetColumnData::str_dict_indices` + `str_dict_values`) — for dict-encoded VARCHAR group columns, per-row lookup is an O(1) array index into `dict_slot_ptrs`, not a memcmp against a string cache. Dropped `GROUP BY region` from 1662 ms to 179 ms.
- **Multi-col GROUP BY: packed uint64 key + scan-wide global dict + `ankerl::unordered_dense::map`** — for a `GROUP BY product, year` where all columns fit ≤ 32 bits, the composite key packs into `uint64_t = (global_idx << 32) | year` and hashes on a flat open-addressing map. Collapsed `GROUP BY product, year` from 5726 ms to 351 ms, then to 177 ms after parallel aggregate.
- **Fused `WHERE` predicate** (`TryCompileSimplePredicate` / `EvalSimplePredicates`) — detects `AGG → FILTER → ParquetScan` with a conjunction of `ColumnRef OP Constant` comparisons on numeric columns, compiles to a flat `std::vector<SimplePredicate>`, applies per row inside the fused aggregate loop. Q5 went from 9202 ms to 270 ms (→ 175 ms after parallel).
- **Parallel aggregate via thread-local `AggState`** — worker threads aggregate into their own `TLSingle`/`TLMulti` state, merged once at the end. Final closure of Q3/Q4/Q5.
- **Per-worker `RGWork` buffer reuse + skip zero-fill** — the consumer-mode workers keep a thread-local `RGWork` across row groups (`ReadColumnInto` uses `resize()` instead of `assign(N, 0)`). Saves ~272 MB of malloc+memset across 80 row groups per query.
- **Cached `ParquetReader` on `TableCatalogEntry`** — `connection.cpp`'s `READ_PARQUET` handler stashes the reader it opened for schema detection; `PhysicalParquetScan::Init()` reuses it. Saves a second Thrift footer parse per query.
- **Dropped `Value min_val` / `Value max_val` writes in the numeric MIN/MAX hot loop** — emit synthesizes a typed `Value` once per group from `sum_min` / `sum_max` using the agg's return type, instead of per-row.
- **Skip `str_data` materialization for dict-encoded VARCHAR group columns** (`ReadColumnInto` `skip_str_data` flag + `PhysicalParquetScan::SetSkipStrData`) — avoids writing 10 M × 16-byte `string_t` entries when the fused GROUP BY only reads dict indices.

### JSON

Starting point: 18.0 s for `SUM(revenue)` on a 1 M-row NDJSON. 64× slower than DuckDB.

- **Rewrote `ParseNDJSON`** — single-pass buffer-loaded parser, dropped the `std::unordered_map<std::string, Value>` per record in favor of a positional `std::vector<Value>` with a `memcmp` key match. Killed per-char `std::string::operator+=` (bulk-append runs + `std::from_chars`).
- **`JSONReader::ReadIntoTable` + `DetectSchemaLight`** — streams directly into DataTable's typed column Vectors; no `Value` boxing, no `BulkLoadRows` copy. `DetectSchemaLight` parses only the first record for schema.
- **mmap the file** — `CreateFileMapping` / `mmap` replaces `ifstream::read`.
- **Parallel parse** — `ReadIntoTable` splits the buffer into line-aligned byte ranges (one per core, clamped 1..8); each worker runs `parse_range` into its own `vector<DataChunk>`; main thread drains.
- **Zero-copy VARCHAR append** (`ColumnData::AppendSlice` + `Vector::GetAuxiliaryPtr()` + `shared_ptr<VectorBuffer> held_bufs_`) — `RowGroup::Append` used to do two `std::string` allocations per VARCHAR cell via a temp Vector with `SetValue`/`GetValue`. New path memcpys the 16-byte `string_t` slice directly and shares the source's `VectorStringBuffer` via `shared_ptr`. Dropped serial append from 320 ms to 35 ms; also benefits CSV and any `DataTable`-backed ingest.
- **`PhysicalJSONScan`** (`TableCatalogEntry::SetJsonPath` + `JSONReader::ReadIntoChunks` + planner dispatch) — eliminates the bulk-load-into-DataTable + rescan roundtrip. `Init()` parses the file into `std::vector<DataChunk>`, `GetData()` emits them to the aggregate.

Progression:

| Stage | SUM(revenue) |
|---|--:|
| Session start | 18.0 s |
| Parser rewrite | 3.0 s |
| Direct-to-DataTable | 1.3 s |
| Parallel parse | 0.80 s |
| mmap | 0.68 s |
| Zero-copy VARCHAR append | 0.32 s |
| **`PhysicalJSONScan`** | **0.24 s** |

### Avro

Starting point: 1.60 s `SUM(revenue)` on 1 M-row Avro; 2× slower than DuckDB.

- **Fixed a pre-existing ordering bug** — `connection.cpp` was calling `GetColumnNames()` before `ReadAll()` (the method that actually parses the file), so the catalog got zero columns and any query referencing them failed with `Column 'X' not found`. Added `AvroReader::DetectSchemaLight()` that parses only the header.
- **`AvroReader::ReadIntoChunks`** — stream-parses Avro data blocks directly into typed `DataChunk` Vector buffers. Numerics go straight into `i64_data[count]` / `f64_data[count]` etc.; strings into `string_t` via `VectorStringBuffer`. No `rows_` vector, no per-cell `Value` boxing.
- **`PhysicalAvroScan` + `SetAvroPath` + planner dispatch** — same template as JSON.

Result: 1.60 s → 140 ms — **5.4× faster than DuckDB's 760 ms**.

### Excel (.xlsx)

Starting point: SlothDB's reader only supported `STORED` (uncompressed) zip entries. Real xlsx files are DEFLATE-compressed; we couldn't open them.

- **Vendored miniz 3.0.2** (`src/storage/miniz.{h,c}`, MIT license). Top-level `project(... LANGUAGES C CXX)`, per-file `/w` compile flag so miniz's own warnings don't trip `/W4`-as-errors.
- **`ExcelReader::ExtractZipEntry`** — handles `compression_method == 8` (DEFLATE) via `tinfl_decompress_mem_to_mem`.
- **Inline-string support** (`t="inlineStr"`) — openpyxl / LibreOffice often write `<is><t>...</t></is>` instead of shared-strings indices.
- **Type widening during parse** — VARCHAR columns widen to BIGINT/DOUBLE on the first numeric cell, instead of silently dropping numeric values to empty strings in `SetValue`.
- **Fixed `connection.cpp` ordering bug** — `GetColumnNames()` called before `ReadAll()`, same bug as Avro.

Result on 1 M-row xlsx `GROUP BY region`: **2.5 s vs DuckDB 3.56 s (1.41× faster)**.

### CSV

Starting point (prior session end): 1.30× slower than DuckDB on `SUM(revenue)`, 4.70× slower on `WHERE ... GROUP BY`. CSV went through `PhysicalFileScan` with a serial per-chunk `ReadChunk` loop; the `WHERE ... GROUP BY` path fell out of the existing parallel GROUP BY branch because `children[0]` was a `PhysicalFilter`.

- **Vectorized `PhysicalFilter::GetData`** (`physical_planner.cpp`) — replaced the per-matching-row `SetValue`/`GetValue` loop with a selection-vector + typed-slice copy per column (`d[i] = s[sel[i]]` for BIGINT / INTEGER / DOUBLE / FLOAT / BOOLEAN / VARCHAR). For VARCHAR, copies `string_t` slices and shares the source's `VectorStringBuffer` via `Vector::SetAuxiliaryPtr(src.GetAuxiliaryPtr())` so pointers stay valid — zero string allocation. Benefits every source, not just CSV.
- **Fused WHERE into parallel GROUP BY** — the existing parallel-GROUP-BY path (per-thread byte-range parse) now also detects `AGG → FILTER → FILE_SCAN`, compiles the predicate via `TryCompileSimplePredicate`, and applies it per-row inside the worker loop via a new `EvalSimplePredicatesChunk` (DataChunk equivalent of `EvalSimplePredicates`). Dropped `WHERE year>=2023 AND quantity>100 GROUP BY region` from 894 ms to 107 ms.
- **Parallel CSV aggregate (no GROUP BY)** — added a branch in the `is_simple_no_group` aggregate path: when `children[0]` is a >16 MB `PhysicalFileScan` (or `PhysicalFilter` wrapping one), split the buffer into N line-aligned byte ranges, each worker runs thread-local SUM/COUNT/AVG/MIN/MAX into its own `AggState` vector, merge once at the end. Also fuses WHERE. Dropped `SUM(revenue)` from 218 ms to 106 ms.
- **`FastCSVReader::ReadIntoChunks`** added (header + .cpp) — parallel mmap-backed parse into a vector of `DataChunk`s. Currently used by tests and available for future callers; the production aggregate paths use in-place thread-local reducers.
- **`Vector::SetAuxiliaryPtr`** (header) — lets one Vector adopt another's string buffer so `string_t` pointers remain valid after a zero-copy copy.

Result: COUNT 5.08×, SUM 1.67×, GROUP BY 1.91×, GROUP BY 2-col 1.70×, WHERE+GROUP BY 1.81× — all faster than DuckDB.

### Supporting infrastructure

- **`ankerl::unordered_dense`** vendored at `include/third_party/` — flat open-addressing hash map, used by the Parquet fused GROUP BY hot paths.
- **`DataTable::AppendRowGroups`** — bulk splice API for future parallel bulk loaders; not yet wired into the default path after profiling showed the extra RG count cost more than the parallelism saved.
- **`ColumnData::AppendSlice`** — zero-copy VARCHAR append; used by all readers that go through `RowGroup::Append`.

---

## Pre-session history

See `git log` for earlier releases (streaming file scan, vectorized aggregation, morsel-driven parallelism, GPU offload scaffolding, etc.).

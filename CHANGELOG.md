# Changelog

All notable changes to SlothDB are documented here.

## 0.2.2

### Performance

- Stats-based row-group pruning for top-N pushdown. Pass 1 of the
  two-pass top-N now reads each row group's max (or min, for ASC)
  from the Parquet footer, sorts RGs best-first, and decodes
  sequentially. The heap converges to the global K-th best after
  one or two RGs; the rest are skipped without ever touching their
  column data on disk. `parquet10m_orderby_top10`: 69 ms to 15 ms.
  Now faster than DuckDB (23 ms), goes from SLOW to WIN.
- Direct `string_t` emit in PhysicalWindow. The vectorised emit
  fast path was going through `Value::VARCHAR` boxing for every
  row. Now writes `string_t` straight into the output vector's
  typed buffer. 10M-row varchar emit drops from ~3 s to ~300 ms.
- Lazy column-major Python results. `QueryResult` now holds the C
  result pointer alive until garbage-collected. Two new methods:
    - `fetchnumpy()` returns a dict of numpy arrays. Numeric
      columns wrap the C buffer via `np.frombuffer` with a
      defensive copy; varchar becomes a numpy object array.
    - `fetchdf()` builds a pandas DataFrame from `fetchnumpy()`,
      bypassing row-tuple materialisation entirely.
  10M-row int conversion drops from ~4 s to ~50 ms.

### Bench scoreboard against DuckDB

13 win, 5 tie, 0 slow on the 18-query suite. Up from 11 / 5 / 2
in 0.2.1.

### Tests

403 of 403 doctest passing on Windows, Linux, macOS.

## 0.2.1

### Fixes

- Avro reader misaligned every nullable column (issue #5). Fields
  declared as `["null", T]` unions ship with a 1-byte union index
  prefix on every value. The reader was skipping that byte, so for
  `["null", {"type":"long","logicalType":"timestamp-millis"}]` the
  index byte was being read as the long value and every subsequent
  column drifted. The reader now reads the union index, treats index 0
  as null, and propagates that into the chunk validity mask.
- Avro logical types: `timestamp-millis`, `timestamp-micros`, `date`,
  `time-millis`, `time-micros`. The schema parser now recognises
  logical-type wrappers, both directly
  (`{"type":"long","logicalType":"timestamp-millis"}`) and inside
  unions. timestamp-millis is rescaled from milliseconds to
  microseconds at decode time to match SlothDB's internal TIMESTAMP
  representation.
- `max(timestamp_col)` and `max(date_col)` returned the first row.
  The aggregate fast-path comparator only handled INTEGER, BIGINT,
  FLOAT, DOUBLE. For TIMESTAMP, DATE, TIME, TIMESTAMP_TZ it returned
  0.0 always, so every row past the first compared as `0 > 0` and
  the first row was the running max forever. Added the date and time
  logical types to both the comparator and the group-key builder.
- DATE, TIMESTAMP, TIME now render as ISO-8601 strings. `Value::ToString`
  was producing raw integers (days, microseconds). DATE now formats as
  `YYYY-MM-DD`, TIMESTAMP as `YYYY-MM-DD HH:MM:SS[.uuuuuu]`, TIME as
  `HH:MM:SS[.uuuuuu]`. The date conversion is reentrant and avoids
  `gmtime`. `Vector::GetValue` no longer drops the LogicalType for
  these types so aggregates keep their typing through the planner.

### Performance

- Chunk-resident QueryResult. SELECT queries now stream typed
  DataChunks straight into `QueryResult::chunks` instead of boxing
  every cell into a Value during result collection. The C API typed
  batch fetch (`slothdb_column_*_buffer`) reads from those chunks via
  memcpy with no intermediate object materialisation. The legacy
  per-cell API (`GetValue`, `ToString`, set ops) lazily materialises
  rows on first access so behaviour is unchanged.

  Bench impact, 10M-row window queries returning to Python:

  | Query             | Before     | After      | DuckDB    |
  |---|---:|---:|---:|
  | parquet10m_window | 18,801 ms  | 12,304 ms  | 7,449 ms  |
  | csv1m_window      | 1,741 ms   | 1,031 ms   | 886 ms    |
  | avro1m_window     | 1,577 ms   | 991 ms     | 1,337 ms  |

- Row-limit pushdown into Parquet scan. A bare `LIMIT N` now clips
  `effective_num_rgs_` in `Init` so workers and the emit loop only
  iterate row groups whose cumulative row count covers the limit.
  `SELECT region, qty FROM file LIMIT 100000` over 10M rows:
  11,500 ms to 132 ms (87x faster). `SELECT * LIMIT 10` 7x faster.
  Skipped when pushdown filters are active.
- Two-pass top-N pass 2 parallelism. Pass 2 now decodes column
  chunks at `(rg, col)` granularity rather than `(rg)`, so a single-RG
  winner set still uses every available core. The key column is no
  longer re-decoded since pass 1 already captured those values.
  `parquet10m_orderby_top10`: 83 ms to 69 ms.

### Tests

- New `sloth-test/test_avro_repro.py` reproduces issue #5 end-to-end.
- New `sloth-test/test_format_smoke.py` runs SELECT, COUNT, MAX,
  WHERE, ORDER BY+LIMIT across CSV, JSON, Parquet, SQLite, Avro, and
  a CSV+JSON join. 22 of 22 passing. (Arrow IPC is skipped: the
  reader uses a SlothDB-bespoke on-disk format, not the Apache Arrow
  Flatbuffers IPC format that pyarrow emits. Pre-existing limitation,
  not a regression.)

## 0.2.0 — performance overhaul, predicate + top-N pushdown, typed Python batch fetch

A wide perf and correctness pass. The 18-query bench against DuckDB went from 3 wins / 6 ties / 9 slow to 11 wins / 5 ties / 2 slow. Several queries that were 50-6000x slower than DuckDB are now within 1.2x or beat it.

### Performance

- **Top-N pushdown for `ORDER BY ... LIMIT N`.** Bounded-heap operator instead of full sort + truncate. 10M-row Parquet `ORDER BY q DESC LIMIT 10`: 420,344 ms -> 119 ms.
- **Two-pass top-N for Parquet.** Pass 1 decodes only the order-by column to find the top-K row indices; pass 2 fetches just those rows from the few RGs that contain them. Pass 2 is parallelised across unique RGs.
- **Parquet row-group statistics pruning.** WHERE clauses pushed into the scan; row groups whose min/max prove no row matches are skipped without decoding. Three latent bugs fixed along the way: stats not propagated from the Thrift parser, fields 5/6 swapped vs the Parquet spec, and cross-type Value comparison silently returning false. `WHERE quantity > 999` (no rows match): 4,984 ms -> 2 ms.
- **Streaming `FROM 'file.parquet'`.** The bare-string-literal Parquet path was materialising every row into an in-memory DataTable before scanning. Now uses the streaming PhysicalParquetScan, identical to `read_parquet()` form. `count(*)`: 90,000 ms -> 0.8 ms.
- **Fused `count(*) WHERE pred OVER PARQUET`.** When the planner sees AGG -> FILTER -> PARQUET with a compileable predicate, runs RunParallelRGs and counts matches per-row inside the worker, skipping PhysicalFilter's row-copy pass entirely. Single-predicate inner loop is hoisted to a per-(type, op) tight loop the compiler auto-vectorises. 4,984 ms -> 76 ms.
- **Vectorised filter executor.** CompareTyped used per-row string comparison of the op name. Hoisted op selection to an enum + branch-free loop. Auto-vectorises when both inputs are all-valid.
- **Typed batch C API.** New `slothdb_column_int32_buffer / int64_buffer / double_buffer / varchar_buffer / validity_buffer` entry points. The Python wrapper now reads one typed buffer per column and transposes via `zip(*cols)` instead of two ctypes calls per cell. SELECT 2 cols x 10M rows: 46 s -> 16 s; row tuples now contain typed values (`int`, `float`) instead of strings.
- **Vectorised window emit.** PhysicalWindow's row-at-a-time emit got a typed-array fast path for the common SELECT cols + ROW_NUMBER/RANK/DENSE_RANK pattern. Plus eager parallel per-partition sort.
- **Identity projection passthrough.** `SELECT * FROM x` now passes chunks unchanged through PhysicalProjection — no per-row ExpressionExecutor overhead. Plus column-pruning hints now forward through projections to file scans, so `SELECT a, b FROM big.parquet ORDER BY a LIMIT 10` decodes only `a` and `b` instead of all 10 columns.
- **URL caching for HTTPS Parquet.** Repeated queries against the same HTTPS Parquet file reuse the cached download instead of re-fetching.

### Bench (10M-row Parquet, vs DuckDB 1.4.3)

| Query | Before this release | After | DuckDB | Delta |
|---|---:|---:|---:|---|
| `count(*)` | 90,000 ms | **0.8 ms** | 2.0 ms | 100,000x faster |
| `count(*) WHERE q > 50` | 4,984 ms | **76 ms** | 65 ms | 65x faster |
| `WHERE q > 999` (prunable) | 4,984 ms | **2 ms** | n/a | 2,500x faster |
| `ORDER BY q DESC LIMIT 10` | 420,344 ms | **119 ms** | 29 ms | 3,500x faster |
| Window `row_number()` | 99,000 ms | 25,000 ms | 11,500 ms | 4x faster |

### SQL surface

- **Recursive `__FILE__` walker.** `WHERE x IN (SELECT ... FROM 'file.csv')`, EXISTS, scalar subqueries, and CTEs containing file literals now bind correctly. Single root-cause fix unlocked 9 coverage tests across `subq`, `cte`, `complex`, `intro` categories.
- **`::` postfix cast** (Postgres-style). `'3.14'::DOUBLE` parses now.
- **`GROUP BY ALL`** (DuckDB-style). Binder fills group keys from non-aggregate select-list entries.
- **`IF(c, t, f)`, `IIF(c, t, f)`, `IFNULL(a, b)`, `NVL(a, b)`** as ternary / null-coalesce helpers.
- **Multi-arg `COALESCE`** correctly resolves to the type of the first non-NULL-typed argument instead of crashing on `SetValue for type NULL`.
- **Tokenizer accepts `[`, `]`, `{`, `}`, `:`** (foundation for upcoming list / struct / map literal support).

### Bug fixes

- Recursive CTE handler now pushes the CTE name to the cleanup list immediately after `CreateTable`. Previously, a failure during recursion left the catalog table behind, breaking the next query that reused the CTE name.
- `BindComparison` now promotes a literal CONSTANT to the column's type when one side is a column ref. Without this, `bigint_col > 50` (where 50 binds as INTEGER) fell into a `std::stod` per-row slow path.
- `python/setup.py`'s `bdist_wheel` override forces a platform-tagged wheel (`py3-none-win_amd64`) instead of pure-Python. Fixes the 0.1.7 wheel that was published as `py3-none-any` and shipped no DLL — the cause of issue #4.

### Cleanup

- `sloth-test/` (local benchmark scratch) removed from tracking and added to `.gitignore`.
- Personal author name removed from public HTML pages and JSON-LD; replaced with project / GitHub references.

### Coverage

170-test SQL battery against DuckDB 1.4.3: 117 pass on both engines (was 108 in the 0.1.7 baseline). Remaining gaps are date / timestamp literals, list / struct / JSON, and a handful of subquery edge cases.

---

## 0.1.7 — `.ask` natural-language SQL, catalog introspection C API, marketing credibility pass

The shell gains a `.ask` command that translates plain English to SQL using a pure rules engine — no model weights, no network, no surprise downloads. 50 KB added to the binary. The C API gains five catalog-introspection functions so any binding (Python, Node, WASM) can enumerate tables + columns without running `information_schema` SQL. **403 tests / 131 513 assertions** green on Windows, Linux, macOS.

### `.ask` — rules-based natural-language → SQL

New shell dot-command that turns English questions into SQL, shows the generated SQL, and prompts `[Y/n]` before running.

```
slothdb> .ask total amount per region
-- SELECT "region", SUM("amount") FROM "sales" GROUP BY "region"
Run? [Y/n] y
```

Pattern coverage in 0.1.7:

- **COUNT** — `how many X`, `count of X`, `count rows in X`, `number of X`, with optional `in YYYY` year filter via auto-detected date column.
- **Aggregates** — `total/sum/average/mean/min/max X` with optional `per/by Y` (`GROUP BY`) and `in YYYY` (`WHERE`).
- **Top-N** — `top N X by Y` / `bottom N X by Y` renders `ORDER BY` + `LIMIT`, direction from the keyword.
- **Select-all** — `rows from X` → `SELECT * FROM X LIMIT 100`.

Schema awareness: singular↔plural table resolution (`sale` → `sales`), exact-column-name matches preferred over synonym routing so `total price` on a two-table schema routes to the table with a column actually named `price` (not the one synonyms would route `price` → `amount` into). Synonym table for the common naming mismatches: `revenue`, `amount`, `total`, `value`, `price`, `cost`, `customer`, `product`, `date`, `region`.

Failure modes are explicit: `UNRESOLVED_TABLE` / `UNRESOLVED_COLUMN` / `NO_MATCH` each with a human message and the offending token. A half-working NL→SQL that silently invents wrong SQL is worse than a rule engine that refuses clearly — we prefer the latter.

Binary-size impact: **+50 KB** on the full build (~1.04 MB → 1.05 MB), **+50 KB** on the edge WASM build (974 KB → 1.00 MB). Zero model weights. Zero new dependencies. An opt-in `.ask --model` mode using Prem-1B-SQL (MIT-licensed, 873 MB lazy download) is planned for 0.1.8 — the default `.ask` stays local, offline, and deterministic.

See [docs/ASK.md](docs/ASK.md) for the full supported-phrasings list and design rationale.

### Catalog introspection C API

Five new functions in `include/slothdb/api/slothdb.h`:

- `slothdb_table_count(conn)` → number of tables
- `slothdb_table_name(conn, i)` → table name
- `slothdb_table_column_count(conn, i)` → columns in table i
- `slothdb_table_column_name(conn, i, j)` → column name
- `slothdb_table_column_type(conn, i, j)` → column type string

The `.ask` shell command is the first consumer; Python / Node / WASM bindings can use the same surface to implement `list_tables()` / `describe_table()` without going through the SQL parser. Thread-local scratch storage means pointers stay valid across cross-function calls (e.g. reading a table name then its columns) but are invalidated on the next call to the same function — documented on the functions.

### Marketing credibility pass

A three-agent copy-review pass (marketing voice, sales conversion, data-analyst credibility) found a ghost claim in the README and landing page: *"1.1× – 8.6× faster than DuckDB, every format, every query"* — but the 0.1.6 benchmark table tops out at **5.43×** (Avro SUM). The 8.6× endpoint didn't exist; any reader who spot-checked found nothing, which collapses trust in the other numbers.

Fixed in three places (README lines 101, 186; docs/docs.html lead + FAQ). Replaced with specific claims: *"3.9× on a 5-query warm JOIN batch"* as the headline, full range stated honestly as *"1.04× – 5.43×, median 1.70×"* in the full-table appendix.

Other marketing changes rolled into this release:

- **Hero reframed** moat-first: *"Live SQL views that follow your files."* (headline) + 3.9× as the proof point.
- **Feature cards reordered** to lead with Live (was Feature-rich). Previous ordering mirrored DuckDB's six cards verbatim, which ceded the moat — `CREATE LIVE VIEW` was hiding inside a generic "feature-rich" card.
- **"If you're using DuckDB today" rewrite** in README. Opens with *"keep using it"* to disarm defensiveness, then names four concrete papercuts (tailing log files, Cloudflare Workers, extension install failures, DuckDB-release ABI breakage).
- **Benchmarks section** on README + landing replaced the 16-row flat wall with three framed stories (JOIN / batch / Avro). Full table demoted to `<details>`. Parquet `SUM(revenue)` 1.04× labeled as tie.
- **CTAs** on landing page now playground-first (`Run a query in the browser →`) with `pip install slothdb` as secondary. Removed the `Read the docs →` graveyard CTA from the hero.
- **Stale `slothdb_0.1.4_amd64.deb` filenames** in README install table → 0.1.7.

### Platform

- CMake project version bumped to 0.1.7. Engine `slothdb_version()` returns `"0.1.7"`.
- Test suite: 381 → 403 (22 new tests for `.ask` pattern coverage, synonyms, refusal modes, determinism).

---

## 0.1.6 — JOIN perf overhaul, ORDER BY correctness, DuckDB-parity metadata, `CREATE LIVE VIEW`, edge build

The JOIN hot path goes from ~1.3× slower than DuckDB to ~2.5× faster on the big × small join benchmark. Two latent ORDER BY / aggregate correctness bugs get closed. Three roadmap-driven metadata features land — `DESCRIBE`, `PRAGMA`, and `VARCHAR(n)` enforcement. And two moat-track features ship for the first time: a file-mtime-cached `CREATE LIVE VIEW` with incremental CSV append, and an `SLOTHDB_EDGE` build that strips to CSV / JSON / Parquet for sub-megabyte WASM bundles. **381 tests / 131 464 assertions** green on Windows, Linux, macOS.

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

### `CREATE LIVE VIEW` — cached views that refresh when the source file changes

First shipped piece of the live-views moat. `CREATE LIVE VIEW app AS SELECT * FROM 'app.log'` caches the result after the first SELECT; subsequent SELECTs compare the file's mtime against the cached value and only re-execute when the file has changed. DuckDB's execution model is snapshot-based and has no equivalent — the only way to refresh a DuckDB view over a file is to re-execute it unconditionally.

Implementation:

- New `LIVE` keyword. Parser accepts `CREATE [OR REPLACE] [LIVE] VIEW`.
- `TableCatalogEntry` gains `is_live_view_`, `watched_file_path_`, `cached_mtime_`, and `cache_valid_` fields.
- `Connection::Query` extracts the single file path from the inner SELECT (`FROM 'path'`, `read_csv`, `read_parquet`, `read_json`, `read_avro`, `read_arrow`, `read_xlsx`). JOINs and multi-file globs are rejected with a clear error for the MVP.
- `expand_view` polls the file's mtime on every SELECT, skips re-execution on cache hit, refreshes on miss.

#### v2 — incremental append for growing CSV logs

Shipped in the same release. For the common log-tail shape — CSV file that only grows, never rewrites — the view now parses only the new bytes instead of re-executing the full SELECT. Append cost scales with `appended_bytes / total_bytes`, not `total_bytes`.

Eligibility is decided at CREATE time: the view must be a pass-through (`SELECT * FROM 'file.csv'` with no `WHERE`, `GROUP BY`, `ORDER BY`, `JOIN`, `DISTINCT`, or `LIMIT`) over a `.csv` or `.tsv` source. Anything else stays on the v1 full-rescan path. The first 64 bytes of the file are stashed at CREATE time as a sentinel — a rewrite-in-place (new header) forces full rescan even when the new size is ≥ the old.

The append path uses `FastCSVReader` in borrowed-buffer mode, seeks past any partial tail line via `FindLineStart`, loops `ReadChunk` appending into the existing `DataTable`. No header re-parse, no re-materialisation of rows already in the cache.

Follow-ups (deferred): JSONL append, WHERE-compatible incremental, background file watcher (inotify / `ReadDirectoryChangesW` / kqueue) to avoid the per-SELECT `stat()`.

### Edge build: `SLOTHDB_EDGE` strips Excel / Avro / Arrow IPC / SQLite

First step of the browser-native moat. A CMake flag that carves the engine down to CSV + JSON + Parquet + core SQL — the readers Cloudflare Workers / Deno Deploy / Vercel Edge targets don't need, and that blow up the WASM bundle past the 1 MB Worker-script cap that blocks DuckDB-wasm.

What's excluded under `-DSLOTHDB_EDGE=ON`:

- `excel_reader.cpp` + `miniz.c` (zip inflate)
- `avro_reader.cpp`
- `arrow_ipc.cpp`
- `sqlite_scanner.cpp`

Source files are dropped from the library via `list(FILTER)` in `src/CMakeLists.txt`. Every call site in `connection.cpp` and `physical_planner.cpp` is `#ifdef`-guarded — excluded readers throw `BinderException` pointing at `@slothdb/wasm` for the full build, and the auto-detect path (`SELECT * FROM 'foo.xlsx'`) emits a clear *"edge build supports CSV / JSON / Parquet only"* error at parse time.

Emscripten flags tighten under EDGE: `-Oz` (size over speed), `-sMALLOC=emmalloc`, `-sFILESYSTEM=0` (read via `fetch()` + `ArrayBuffer`, not the emscripten FS), `--closure=1` for JS minification.

On a native MSVC build the static library shrinks ~6% (the linker already strips most dead code). The real WASM win is larger — Emscripten's DCE is looser, and the `-Oz` + closure pass compound once the template-heavy readers are out. Actual bundle size numbers land with the first Emscripten build using this config. See `docs/EDGE_BUILD.md`.

### Platform

- CMake project version bumped to 0.1.6. Engine `slothdb_version()` returns `"0.1.6"`.
- Test suite: 363 → 381 (18 new tests covering DESCRIBE, PRAGMA, VARCHAR(n), CREATE LIVE VIEW v1 + v2).

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

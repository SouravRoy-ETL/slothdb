# SlothDB Roadmap

_Last updated: v0.1.4. Sourced from two feature-gap research passes._

This document is a standing record of what SlothDB is missing vs DuckDB (parity work), where SlothDB could stake unique ground DuckDB can't follow (identity work), and the recommended sequencing between the two. It's meant as a reference for contributors picking up work and for the maintainer deciding what to build next.

---

## 1. Where SlothDB is today (v0.1.4)

Shipped and working across Windows / Linux / macOS:

- 130+ SQL features: SELECT, joins (inner/left/right/outer/natural/USING), CTEs (recursive), set ops, window functions, QUALIFY, MERGE
- 70+ scalar functions covering string, math, date/time, regex, trigonometric, aggregate
- Seven file format readers built into core: Parquet, CSV, JSON, Avro, Arrow IPC, SQLite, Excel
- HTTP(S) remote reads and public-bucket `s3://` URLs work in any SQL path
- Extended `DATE_TRUNC` (WEEK / QUARTER / DECADE / MINUTE / SECOND / MILLISECOND) and four new scalars: `MONTHNAME`, `DAYNAME`, `LAST_DAY`, `MAKE_DATE`
- CLI shell, Python package, C/C++ embedding with a stable C ABI
- 359 tests, 131,382 assertions, green on all three platforms
- Benchmarks: 1.04×–8.6× faster than DuckDB on every format in the shipped suite

Known hard gaps:

- No `LIST`, `STRUCT`, `MAP`, `INTERVAL` (as a first-class type), `JSONB`, `BITSTRING`, `UUID`, `ENUM`, `TIMESTAMPTZ`
- No `ATTACH` / `DETACH`, no `INSTALL` / `LOAD` extension registry, no `SUMMARIZE`, no `DESCRIBE SELECT`, no `EXPLAIN ANALYZE` with runtime stats
- No AWS SigV4, no HTTP range-request streaming (whole file is downloaded to a temp file)
- No `PIVOT` / `UNPIVOT`, no `ROLLUP` / `CUBE` / `GROUPING SETS`
- No prepared statements, no Python asyncio
- `VARCHAR(n)` parses but the length is not enforced
- Extension ABI registers scalar functions only — no table or aggregate function registration

---

## 2. Parity work — close the gap so DuckDB users stop bouncing

Ranked by (user impact) ÷ (implementation complexity).

### Cheap & high-impact (single session each)

| # | Gap | DuckDB syntax | LOC | Why it matters |
|---|---|---|---|---|
| 1 | **DESCRIBE + SUMMARIZE** | `DESCRIBE SELECT * FROM t;` · `SUMMARIZE t;` | ~250 | First command every analyst runs on a new file. Currently throws. |
| 2 | **PRAGMA introspection** | `PRAGMA table_info('t');` · `pragma_database_list()` | ~200 | DBT / Metabase / SQLAlchemy / dbeaver won't introspect without these. Blocks BI integrations. |
| 3 | **EXPLAIN ANALYZE** | `EXPLAIN ANALYZE SELECT ...;` | ~300 | Perf conversations, benchmark comparisons to DuckDB. Users expect it on day one. |
| 4 | **VARCHAR(n) enforcement** | `CAST(x AS VARCHAR(5))` | ~80 | Parser already reads `n`; executor silently ignores it. Prevents silent data corruption. |
| 5 | **MAKE_TIMESTAMP / AGE / EPOCH** | `MAKE_TIMESTAMP(y,m,d,h,mi,s)` · `AGE(ts1, ts2)` | ~120 | Slots in next to the existing date scalar block. Trivial parity win. |

**Total: ~950 LOC, fits in 3-4 sessions combined, measurable immediate credibility lift.**

### Mid-complexity (one longer session or two)

| Gap | DuckDB syntax | LOC | Notes |
|---|---|---|---|
| **Prepared statements** | `conn.prepare("... WHERE id=$1")` | ~350 | Unlocks SQLAlchemy, high-QPS repeat-query workloads. Planner caches physical plan keyed by SQL. |
| **PIVOT / UNPIVOT (desugared)** | `PIVOT sales ON year USING SUM(amt);` | ~700 | Rewrite to `GROUP BY + CASE` in binder; full PIVOT later. |
| **GROUPING SETS / ROLLUP / CUBE** | `GROUP BY CUBE(region, year)` | ~400 | Aggregate-level rewrite; currently zero tokens in parser. |
| **ATTACH multi-database** | `ATTACH 'data.db' AS d; SELECT * FROM d.users;` | ~400 | Catalog extension — per-schema `Catalog`. `sqlite_scanner` already exists. |

### Expensive & strategic (multi-week, but transformational)

1. **Range-request Parquet streaming over HTTPS / S3** (~800 LOC, 2-3 sessions)
   Current state: `SELECT * FROM 'https://bucket/5gb.parquet'` downloads the whole file into RAM via `HTTPClient::Get`. DuckDB's killer feature is fetching only the footer (~40 MB) plus selected column chunks — essential for real S3 lakehouse use.
   Work: introduce a `RangeReadableFile` abstraction; add HTTP `Range:` header in `src/storage/http_client.cpp`; refactor `src/storage/parquet.cpp` footer reader and column-chunk reader to seek instead of buffer-whole-file.
   After this lands, SlothDB is genuinely usable for >1 GB remote Parquet. Follow-up: AWS SigV4 (~250 LOC) for private buckets.

2. **LIST type + UNNEST** (~2,500 LOC, multi-week)
   Unlocks `ARRAY_AGG`, JSON nested arrays, the list-handling ~15 downstream scalar functions. Big, transformational for JSON workflows. Needs vector storage for variable-width children, printer support, CAST rules, and a new `UNNEST` operator.

3. **Prepared statements + Python asyncio** (~350 + 400 LOC)
   Takes SlothDB from CLI/notebook tool to viable backend for web apps and DBT pipelines.

---

## 3. Identity work — where DuckDB can't follow

The research was deliberately brutal. Most "novel" differentiator ideas for an embedded OLAP DB are feature-race graveyards. Three bets actually have a shot.

### Bet 1 — WASM playground at `try.slothdb.org`  (2-4 weeks)

**This is a distribution weapon, not a feature.** At 8 stars, the bottleneck is not capability — it's that nobody knows SlothDB exists. `shell.duckdb.org` is probably responsible for more DuckDB awareness than any blog post they've written. A URL that lets someone paste SQL and hit a 1M-row CSV in under 200 ms with zero install changes every HN comment, Reddit reply, and DM about the project.

Week-1 shippable scope (5 working days for the maintainer):

1. Day 1–2: `cmake -DCMAKE_TOOLCHAIN_FILE=emscripten.toolchain`. Stub out `http_client.cpp` and `file_handle.cpp` (use MEMFS). Disable threads. Get `slothdb.wasm` to link.
2. Day 3: Minimal HTML/JS harness. `<textarea>` for SQL, `<input type="file">` that loads to MEMFS as `/data/upload.csv`, results rendered as an HTML table.
3. Day 4: Pre-load the existing 1M-row demo CSV (gzipped) so the landing page works with zero user action.
4. Day 5: Deploy to Cloudflare Pages as `try.slothdb.org`. Write the announcement post ("SlothDB now runs in your browser — 1M-row GROUP BY in 180 ms, no install").

This is a legitimate Show HN / r/programming post on its own merits and unlocks every other differentiator shipped afterward.

### Bet 2 — Stable-ABI extension registry  (3-6 weeks)

SlothDB is **the only embedded OLAP DB with a stable C ABI for extensions**. DuckDB literally cannot compete here without a major architectural rewrite. The narrative is the product: "`INSTALL crypto; LOAD crypto;` — still works next release. DuckDB extensions break every minor version."

Minimum viable shape: `extensions.slothdb.org` serving a JSON manifest + CDN'd `.dll/.so/.dylib`. Five opinionated launch extensions:

- `regex_enrich` (precompiled PCRE, faster than DuckDB's built-in regex)
- `http_udf` (call an HTTP endpoint from SQL — sink or source)
- `crypto` (bcrypt, SHA family, HMAC)
- `ulid` / `nanoid` (generation + parsing)
- `dns_lookup` (IP enrichment for log analysis)

Each is 100-300 LOC against the existing C ABI. Most of the work is the packaging / signing / CLI (`slothdb install regex_enrich`), not the extensions themselves.

### Bet 3 — Live-refreshing file views  (2-3 weeks)

`CREATE LIVE VIEW logs AS SELECT * FROM 'app.log'` that **auto-invalidates on file change**. DuckDB is snapshot-only. Nobody in the embedded-OLAP space does this well, and it slots cleanly into SlothDB's local / edge BI positioning.

MVP: `inotify` / `ReadDirectoryChangesW` + a dirty flag on the table catalog entry; re-scan lazily on next `SELECT`. No streaming engine, no incremental state — just freshness, which is what people actually want 90% of the time. The append-only streaming variant (don't re-read the whole file, only the new bytes) is the v2.

### Ideas explicitly rejected

The following looked tempting but would be bad bets for a solo maintainer at this stage:

- **Vector search / embeddings** — would enter a crowded fight with LanceDB, Chroma, Turbopuffer, pgvector, sqlite-vec, and DuckDB-vss. HNSW done right is 2-3 months of work just for the index. The brute-force MVP is a toy nobody will use in production.
- **ML primitives in SQL** (`linear_regression()`, `kmeans_predict()`) — MADlib graveyard. Real ML users reach for `sklearn.fit()`. Months of work on something that loses on every axis.
- **Geospatial** (`GEOMETRY`, `ST_*` functions) — wrapping GEOS / GDAL is 500k+ LOC of dependency that kills the "8 MB binary" line. Re-implementing is years of work.
- **Full streaming / incremental view maintenance** — Materialize-style IVM is PhD-tier. The local-use-case subset is served by bet 3 (live views) instead.
- **Delta Lake / Iceberg readers** — formats evolve fast with 50+ edge cases. Only helps users who already have a lake, which is exactly the crowd already running Spark / Trino / DuckDB.
- **`jq`-style JSON paths** and **time-series functions** are fine as single-afternoon additions to pad the features table, but neither creates a memorable identity on its own.

---

## 4. Recommended sequencing

**Parallel tracks. Don't serialize.**

### Track A — parity (sprint)

Ship the four cheap wins in one session each over a week:

1. `DESCRIBE` + `SUMMARIZE`
2. `PRAGMA` introspection functions
3. `EXPLAIN ANALYZE`
4. `VARCHAR(n)` enforcement

Each is demo-visible. Each unblocks a class of users. Combined ~830 LOC, ~4-6 hours of implementation per item.

### Track B — identity (commit to one)

Pick the WASM playground (bet 1). It's the only item on either list where the outcome is "people discover the project," not "people who already know the project see a new feature." Range-request Parquet streaming is strategically important but should ship **after** the WASM playground — because the playground is what creates the audience that'd benefit from Parquet streaming in the first place.

### Explicitly defer

- `LIST` / `STRUCT` types — come back in v0.2 or when there's a second contributor
- `PIVOT` / `UNPIVOT` — pad the features table when there's idle time
- Vector search, ML, geo, Delta / Iceberg — do not start

---

## 5. Research methodology

This document is the distilled output of two research passes run as tool-delegated agents:

- **Gap analysis** — surveyed current codebase, cross-referenced DuckDB documented surface, ranked 8 remaining parity gaps by impact ÷ complexity, flagged cheap-and-high-impact vs expensive-and-strategic.
- **Differentiation analysis** — evaluated 10 candidate "make it unique" directions against feasibility for a solo maintainer over 1-3 months and strategic fit with current positioning. Deliberately rejected 6 of the 10 as feature-race graveyards.

When this doc is next updated, re-run both passes. Specifics shift as the project ships.

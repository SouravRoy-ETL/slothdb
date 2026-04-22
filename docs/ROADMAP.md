# Roadmap

Rough, rolling. I bump it when a release ships or when my own thinking changes. If you're a contributor looking for something to pick up, the "small stuff" list is a good place to start — drop me an issue first so we don't duplicate work.

Last pass through: v0.1.5.

---

## Where the project is right now

The v0.1.5 release tag is what you get from `pip install slothdb`, `npm install @slothdb/wasm`, or a download from the releases page. It ships:

- Core SQL: SELECT, all the joins (including natural / USING), CTEs (recursive), set ops, window functions, QUALIFY, MERGE, COPY. ~130 features.
- ~70 scalar functions — string, math, date/time, regex, trig, aggregates.
- Seven file-format readers built into the core binary: Parquet, CSV, JSON, Avro, Arrow IPC, SQLite, Excel. No extension to install first.
- `https://` and `s3://` URLs work anywhere a file path does (public S3 only for now — no SigV4 yet).
- CLI shell, Python wheel, npm package, stable C ABI for embedding.
- Browser playground at slothdb.org/playground — full engine compiled to WebAssembly.
- 363 tests, 131,408 assertions, green on Windows / Linux / macOS.
- Faster than DuckDB on every format I've benchmarked (1.04×–8.6×). Take this with a grain of salt — benchmarks favour whoever picked the queries.

What's genuinely missing and hurts:

- `LIST` / `STRUCT` / `MAP` aren't first-class types. `INTERVAL`, `UUID`, `ENUM`, `TIMESTAMPTZ`, `JSONB`, `BITSTRING` — same.
- `DESCRIBE`, `SUMMARIZE`, `PRAGMA table_info()`, `EXPLAIN ANALYZE` all throw. This is the first thing most people try.
- `ATTACH` / `DETACH`. No multi-database sessions.
- `PIVOT` / `UNPIVOT` / `ROLLUP` / `CUBE` / `GROUPING SETS`.
- No prepared statements. No Python asyncio.
- `VARCHAR(n)` parses the `n` but the executor ignores it.
- Range-request streaming over HTTP — right now `FROM 'https://bucket/huge.parquet'` downloads the whole file first. Fine for small things, useless for lake-scale.
- Extension ABI only registers scalar functions, not table/aggregate ones.

---

## Small stuff (one-sitting each)

These are the easy wins. I'm doing them in roughly this order.

- **DESCRIBE / SUMMARIZE**. First thing every analyst runs on a new file. Currently throws. Maybe a couple hundred lines.
- **PRAGMA introspection** (`pragma_table_info`, `pragma_database_list`). DBT, Metabase, SQLAlchemy, DBeaver — they all introspect via PRAGMAs. Without this you can't plug SlothDB into a BI tool.
- **EXPLAIN ANALYZE**. The plan side exists; wiring runtime counters through the executor is the work.
- **VARCHAR(n) enforcement**. Parser already reads the length; the executor just has to check it. Prevents silent truncation/corruption that nobody notices until production.
- **MAKE_TIMESTAMP / AGE / EPOCH**. Three more date functions, slots into the existing table of 0.1.4 additions.

Rough guess: all of the above in about a week of real work.

## Middle stuff (weekend-ish each)

- **Prepared statements.** Unblocks SQLAlchemy and any high-QPS workload that re-runs the same query shape. Planner needs a cache keyed on SQL text + statement identity.
- **PIVOT / UNPIVOT**, desugared to GROUP BY + CASE in the binder first. Full PIVOT later.
- **GROUPING SETS / ROLLUP / CUBE**. Aggregate-node rewrite; zero tokens in the parser right now.
- **ATTACH / DETACH**. Catalog already supports multiple tables; needs a schema layer and the parser entry. The SQLite scanner shows the pattern for per-database bindings.

## Big stuff (multi-week)

- **Range-request Parquet over HTTP.** The current `HTTPClient::Get` downloads end-to-end. For Parquet this is wrong — you want the footer (a few MB) then only the column chunks the query touches. Needs a `RangeReadableFile` abstraction, `Range:` header wiring, and refactoring the Parquet footer + column reader to seek instead of buffer-whole-file. After this, >1 GB remote Parquet actually works. Follow-up item is AWS SigV4 for private buckets — not huge but can wait.
- **LIST type and UNNEST.** Transformational for JSON workflows. Unlocks `ARRAY_AGG` and ~15 list-handling scalar functions. Needs variable-width child storage in Vector, printer support, CAST rules, and a new UNNEST operator. This one's at least a couple of weeks.
- **Python asyncio wrapper.** A thin layer on the existing C API. Not huge code but it's the difference between "works in a notebook" and "works as a DBT warehouse backend".

---

## Longer bets

These I'm less sure about. Each is a week-ish of prototyping before I know if it's worth another month.

**Stable-ABI extension registry.** SlothDB has a stable C ABI for scalar functions — DuckDB doesn't; their extensions break on minor version bumps. If I can package five useful extensions (regex, crypto, ULID generation, HTTP UDF, DNS lookup) and make `slothdb install crypto; LOAD crypto;` work across releases, the "extensions just keep working" story is genuinely a differentiator. Most of the work is packaging, signing, and a small CDN for the binaries — the extensions themselves are 100–300 lines each.

**Live-refreshing file views.** `CREATE LIVE VIEW logs AS SELECT * FROM 'app.log'` that re-reads when the file changes. `inotify` on Linux, `ReadDirectoryChangesW` on Windows, `kqueue` on macOS. MVP is just a dirty flag and a lazy re-scan on the next SELECT; append-only streaming (only read new bytes) is v2. DuckDB is snapshot-only and nobody in the embedded-OLAP space does this well.

I'm not planning to start either until the small+middle list above is mostly done, but if you're a contributor who wants to take one on, reach out.

---

## Things I'm explicitly not doing

Recording these so they stop eating my attention:

- **Vector search / embeddings.** LanceDB, Chroma, Turbopuffer, pgvector, sqlite-vec, duckdb-vss — crowded. HNSW done properly is months of index work. A brute-force MVP is a toy that doesn't survive production. Not my fight.
- **In-SQL ML primitives** (`linear_regression()`, `kmeans_predict()`). MADlib is the warning. The people who'd use this reach for sklearn; the people who wouldn't don't exist.
- **Geospatial.** GEOS + GDAL is half a million lines of dependency that destroys the "small binary" pitch. Reimplementing PostGIS is years.
- **Full streaming / IVM** (Materialize-style). PhD-tier. The tiny useful slice of it (local freshness) is covered by the live-views idea above.
- **Delta Lake / Iceberg readers.** Formats that evolve fast and mostly help users who already have Spark / Trino / DuckDB. If someone wants these, they probably don't want SlothDB.
- **`jq`-style JSON paths and random time-series functions.** Fine as one-afternoon adds but don't change anyone's mind about the project.

---

## What actually ships next

Current plan for 0.1.6, if nothing sidetracks me:

1. `DESCRIBE` + `SUMMARIZE`
2. `PRAGMA table_info` + `pragma_database_list`
3. `EXPLAIN ANALYZE` with runtime counters
4. `VARCHAR(n)` length enforcement
5. Maybe `MAKE_TIMESTAMP` / `AGE` / `EPOCH`

If I'm honest with myself, (1)–(4) is realistic. Five is optional.

For 0.2, the list is prepared statements and range-request HTTP Parquet. Everything else is noise until those two land.

---

This doc should be taken as rough direction, not a commitment. If you're thinking of contributing, open an issue or discussion before starting — saves duplicated work, and some items have unwritten context about why the obvious approach doesn't work.

<p align="center">
  <img src="https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/assets/hero.svg" alt="SlothDB" width="100%">
</p>

<h3 align="center">An experimental embedded SQL engine.</h3>

<p align="center">
  SlothDB is a from-scratch C++20 embedded SQL database in active development. Query Parquet, CSV, JSON, Avro, Arrow, Excel, and SQLite files directly with SQL, in-process. Early-stage; read the <a href="https://github.com/SouravRoy-ETL/slothdb#status">Status</a> section on GitHub before treating any performance numbers as final.
</p>

<p align="center">
  <a href="https://discord.gg/XJWyGmX5G">
    <img src="https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/assets/discord-cta.svg" alt="Join the SlothDB Discord" width="340">
  </a>
</p>

<p align="center">
  <a href="https://pypi.org/project/slothdb/"><img src="https://img.shields.io/pypi/v/slothdb?color=3775A9&logo=pypi&logoColor=white" alt="PyPI"></a>
  <a href="https://pepy.tech/project/slothdb"><img src="https://static.pepy.tech/badge/slothdb" alt="Downloads"></a>
  <a href="https://pepy.tech/project/slothdb"><img src="https://static.pepy.tech/badge/slothdb/month" alt="Downloads/month"></a>
  <a href="https://pypi.org/project/slothdb/"><img src="https://img.shields.io/pypi/pyversions/slothdb" alt="Python versions"></a>
  <a href="https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml"><img src="https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License"></a>
  <a href="https://peerpush.net/p/slothdb"><img src="https://peerpush.net/p/slothdb/badge.png" alt="PeerPush"></a>
</p>

---

## Try it in 60 seconds

```bash
pip install slothdb
python -c "import slothdb; slothdb.demo()"
```

Generates a 100 000-row CSV, runs three queries, and prints the side-by-side with DuckDB shown above. No files to find, no setup.

## Using your own files

```python
import slothdb
db = slothdb.connect()
df = db.sql("SELECT region, SUM(revenue) FROM 'sales.parquet' GROUP BY region").fetchdf()
```

No server. No import step. No CREATE TABLE. Point SQL at files on disk.

## What's new in 0.2.7

- Four benchmark-fitted shortcuts deleted from the engine. Two were returning wrong results on inputs outside the benchmark. The previous "33 of 43 beats DuckDB, up to 5×" claim was removed alongside them. Public discussion of the engine's architecture: [ClickHouse/ClickBench#930](https://github.com/ClickHouse/ClickBench/issues/930). Treat any remaining performance numbers as anecdotal.
- DATE and TIMESTAMP columns render as ISO strings (`2013-07-02`, `2013-07-15 12:40:00`) instead of raw epoch integers.
- `slothdb_version()` and the shell `--version` report the release tag.
- 424 doctest cases pass.

## What's new in 0.2.5

- Nested aggregates work everywhere. `ROUND(AVG(x))`, `AVG(x) + 1`, `SUM(x) / COUNT(*)`, `CAST(SUM(y) AS DOUBLE)` and similar shapes that wrap an aggregate inside a scalar function or arithmetic used to throw "Function execution for: AVG". Fixed.
- `ORDER BY` by aggregate alias works. `SELECT region, COUNT(*) AS cnt ... ORDER BY cnt DESC` no longer silently sorts by column 0.
- Arithmetic type promotion fixed. `AVG(x) + 1` no longer drops the `+1` and `AVG(x) / COUNT(*)` no longer returns `inf`.
- 408 unit tests, 131,537 assertions, green on Windows, Linux, macOS.

## Why SlothDB?

Same embedded model as DuckDB and SQLite. You link it into your process and point SQL at files. Different defaults:

- **7 file formats built in** - Parquet, CSV, JSON, Avro, Arrow, SQLite, Excel. DuckDB needs extensions for Avro and SQLite.
- **Stable C ABI.** Numeric error codes don't shift between releases. Bindings built against 0.1.x keep working.
- **~1-4 MB single binary**, fully self-contained.

Performance vs DuckDB is competitive on some queries and worse on others. The GitHub README's [Status](https://github.com/SouravRoy-ETL/slothdb#status) section has the architecture context behind the numbers.

## Quickstart

```python
import slothdb

# In-memory
db = slothdb.connect()

# Query files directly
db.sql("SELECT * FROM 'data.csv' WHERE score > 90").show()
db.sql("SELECT COUNT(*) FROM 'logs.parquet'").show()
db.sql("SELECT * FROM read_json('events.json') LIMIT 5").show()
db.sql("SELECT * FROM sqlite_scan('app.db', 'users')").show()

# Persistent database
db = slothdb.connect("analytics.slothdb")

# DataFrame integration
df = db.sql("SELECT region, SUM(revenue) FROM 'sales.csv' GROUP BY region").fetchdf()
```

## What's not production-ready yet

- No multi-writer transactions (single writer, crash-safe checkpoint).
- No distributed execution. Single-node embedded engine.
- No secondary indexes. Scan-based execution; zone-map pruning helps on sorted data, but no B-tree / hash index for point lookups.
- Window-function coverage is partial. Plain OVER / PARTITION BY works; `ROWS BETWEEN ...` frames and cumulative `SUM OVER (ORDER BY)` shapes have known gaps.
- Authenticated S3 not implemented. `s3://` URLs work for anonymous public-bucket reads only.
- Some SQL corners still surprise you. Open an [issue](https://github.com/SouravRoy-ETL/slothdb/issues) with a repro.
- 0.2.x, about a year old. Treat as beta.

## Performance

Performance varies query-to-query: SlothDB is competitive or faster on some shapes (native Avro decode, CSV `COUNT(*)`, small joins) and slower on others (high-cardinality multi-column `GROUP BY`, the generic non-handler path). The GitHub README's [Status](https://github.com/SouravRoy-ETL/slothdb#status) section explains the architectural reason and what's being changed.

`pip install slothdb && python -c "import slothdb; slothdb.demo()"` runs a 3-query side-by-side against whatever DuckDB version is installed on your machine, so you can see the comparison locally.

## Community

There's a Discord: **[discord.gg/XJWyGmX5G](https://discord.gg/XJWyGmX5G)**. Bug reports, install help, weird query plans, "is this slower than it should be", feature ideas - any of it. The maintainer reads everything. GitHub issues are still the canonical tracker; the server is for the questions that come before you file one.

## Links

- **Source**: https://github.com/SouravRoy-ETL/slothdb
- **Discord**: https://discord.gg/XJWyGmX5G
- **Changelog**: https://github.com/SouravRoy-ETL/slothdb/blob/main/CHANGELOG.md
- **Issues**: https://github.com/SouravRoy-ETL/slothdb/issues
- **SQL reference**: https://github.com/SouravRoy-ETL/slothdb/blob/main/docs/DOCUMENTATION.md

## License

[MIT](https://github.com/SouravRoy-ETL/slothdb/blob/main/LICENSE)

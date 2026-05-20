<p align="center">
  <img src="https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/assets/hero.svg" alt="SlothDB" width="100%">
</p>

<h3 align="center">Run analytics faster.</h3>

<p align="center">
  SlothDB is an embedded SQL database that runs everywhere: on your laptop, on a server, and in the browser. Built from scratch as a DuckDB alternative. <b>Up to 5x faster</b> on real workloads (138 ms vs 540 ms on a 5-query warm JOIN batch; 5.43x peak on Avro SUM; 16-query suite median 1.70x). Built-in readers for Parquet, CSV, JSON, Avro, Arrow, Excel, and SQLite.
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

<p align="center">
  <img src="https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/assets/demo.svg" alt="SlothDB 60-second demo" width="90%">
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

- Re-measured ClickBench honestly. SlothDB completes 40 of the 43 official queries and is faster than DuckDB on 29 of those 40, geomean 1.24x. The previous "33 of 43" framing counted 8 queries where DuckDB rejected the input as wins for SlothDB; that's gone. Raw per-query times: [official_results.md](https://github.com/SouravRoy-ETL/slothdb/blob/main/bench/clickbench/official_results.md).
- Four benchmark-fitted shortcuts deleted from the engine. Two of them were producing wrong results on inputs outside the benchmark; that's fixed.
- DATE and TIMESTAMP columns render as ISO strings (`2013-07-02`, `2013-07-15 12:40:00`) instead of raw epoch integers.
- `slothdb_version()` and the shell `--version` now report the correct release number.
- 424 doctest cases pass.

## What's new in 0.2.5

- Nested aggregates work everywhere. `ROUND(AVG(x))`, `AVG(x) + 1`, `SUM(x) / COUNT(*)`, `CAST(SUM(y) AS DOUBLE)` and similar shapes that wrap an aggregate inside a scalar function or arithmetic used to throw "Function execution for: AVG". Fixed.
- `ORDER BY` by aggregate alias works. `SELECT region, COUNT(*) AS cnt ... ORDER BY cnt DESC` no longer silently sorts by column 0.
- Arithmetic type promotion fixed. `AVG(x) + 1` no longer drops the `+1` and `AVG(x) / COUNT(*)` no longer returns `inf`.
- 408 unit tests, 131,537 assertions, green on Windows, Linux, macOS.

## Why SlothDB?

Same embedded model as DuckDB and SQLite. You link it into your process and point SQL at files. Different defaults:

- **7 file formats built in** - Parquet, CSV, JSON, Avro, Arrow, SQLite, Excel. DuckDB needs extensions for Avro and SQLite.
- **Faster than DuckDB on real workloads.** 5-query warm JOIN batch: 138 ms vs 540 ms (3.9x). Peak speedups: 5.43x on Avro SUM, 5.08x on CSV COUNT(\*), 2.83x on Parquet COUNT(\*). Median across the 16-query suite: 1.70x. [Full numbers on GitHub.](https://github.com/SouravRoy-ETL/slothdb#performance)
- **Stable C ABI.** Numeric error codes don't shift between releases. Bindings built against 0.1.x keep working.
- **~1-4 MB single binary**, fully self-contained.

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

| Format | Query | SlothDB | DuckDB | Speedup |
|---|---|--:|--:|:-:|
| Parquet | `COUNT(*)` | 12 ms | 34 ms | **2.83×** |
| CSV | `COUNT(*)` | 33 ms | 170 ms | **5.08×** |
| CSV | `GROUP BY region` | 100 ms | 191 ms | **1.91×** |
| JSON | `SUM(revenue)` | 242 ms | 314 ms | **1.30×** |
| Avro | `SUM(revenue)` | 140 ms | 760 ms | **5.43×** |
| Avro | `GROUP BY region` | 170 ms | 800 ms | **4.71×** |

1M-row dataset, warm cache, 5-run median. [Full 15-query table + methodology →](https://github.com/SouravRoy-ETL/slothdb#performance)

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

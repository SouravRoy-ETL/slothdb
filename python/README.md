<p align="center">
  <img src="https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/assets/hero.svg" alt="SlothDB" width="100%">
</p>

<h3 align="center">SlothDB is a fast, in-process SQL database for your local data files.</h3>

<p align="center">
  A drop-in alternative to DuckDB with built-in readers for Parquet, CSV, JSON, Avro, Arrow and SQLite — <b>1.1×–8.6× faster</b> on every benchmark query.
</p>

<p align="center">
  <a href="https://pypi.org/project/slothdb/"><img src="https://img.shields.io/pypi/v/slothdb?color=3775A9&logo=pypi&logoColor=white" alt="PyPI"></a>
  <a href="https://pypi.org/project/slothdb/"><img src="https://img.shields.io/pypi/dm/slothdb?color=3775A9&label=downloads" alt="Downloads"></a>
  <a href="https://pypi.org/project/slothdb/"><img src="https://img.shields.io/pypi/pyversions/slothdb" alt="Python versions"></a>
  <a href="https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml"><img src="https://github.com/SouravRoy-ETL/slothdb/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License"></a>
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

## Why SlothDB?

Same embedded model as DuckDB and SQLite — link it into your process, point SQL at files. Different defaults:

- **7 file formats built in** — Parquet, CSV, JSON, Avro, Arrow, SQLite, Excel. DuckDB needs extensions for Avro and SQLite.
- **1.1–8.6× faster than DuckDB** on a 1M-row benchmark across 15 queries. JSON parse is 8.6×, Avro SUM is 5.4×, CSV `COUNT(*)` is 5.1×. [Full numbers on GitHub →](https://github.com/SouravRoy-ETL/slothdb#performance)
- **Stable C ABI** — extensions don't break across releases.
- **~8 MB single binary**, fully self-contained.

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

- No multi-writer transactions (single-writer, crash-safe checkpoint).
- No distributed execution — single-node embedded engine.
- Some SQL corners still surprise you (open an [issue](https://github.com/SouravRoy-ETL/slothdb/issues)).
- v0.1.4, ~6 months old. Treat as beta.

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

## Links

- **Source**: https://github.com/SouravRoy-ETL/slothdb
- **Changelog**: https://github.com/SouravRoy-ETL/slothdb/blob/main/CHANGELOG.md
- **Issues**: https://github.com/SouravRoy-ETL/slothdb/issues
- **SQL reference**: https://github.com/SouravRoy-ETL/slothdb/blob/main/docs/DOCUMENTATION.md

## License

[MIT](https://github.com/SouravRoy-ETL/slothdb/blob/main/LICENSE)

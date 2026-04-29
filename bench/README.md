# bench/

Reproducible benchmarks for SlothDB against DuckDB. The runner is
`bench/run.py`; suites live in subdirectories with their own `queries.sql`
and a `README.md` describing the data setup.

## How the runner works

```
python bench/run.py \
    --queries <path/to/queries.sql> \
    --table   <bare identifier in the queries> \
    --data    <path or glob to data file(s)> \
    --slothdb <path to slothdb binary> \
    --duckdb  <path to duckdb binary> \
    [--runs N] [--warmup] [--timeout SEC] [--out results.md] [--skip i,j,...]
```

The runner reads the queries file (one statement per line, `--` comments
stripped) and substitutes every occurrence of the bare `<table>` identifier
with `'<data>'` before sending the query to each engine. So a queries file
written with `FROM hits` runs against any parquet/CSV/Avro file by passing
the same `--table hits --data /some/file.parquet`.

Times come from `time.perf_counter()` around `subprocess.run([exe, "-c", q])`,
median of `--runs` runs (default 3). Pass `--warmup` to discard a warmup
run on each side before timing.

The summary line at the end reports median and geomean speedup over DuckDB.

## Suites

- `bench/sloth16/` &mdash; 16-query mixed suite (point counts, GROUP BY,
  filtered aggregates, top-N, window functions, nested aggregates). Runs
  against any of the `real-life-testing/sales_*.{csv,parquet}` files.
- `bench/clickbench/` &mdash; the official 43-query ClickBench suite,
  verbatim from `ClickHouse/ClickBench/main/_data/queries.sql`. Requires
  the `hits.parquet` dataset; see `bench/clickbench/README.md` for the
  download URL and exact run command.
- `bench/h2o/` &mdash; H2O.ai groupby benchmark, 5 queries on a 1e7-row
  dataset. (Wired to be added; runner already supports it.)

## Adding a suite

Drop a `queries.sql` file in `bench/<name>/` with one statement per line
using a single bare identifier (e.g. `data`, `hits`, `tbl`) for the input
table. The runner substitutes that token. Aim for representative shapes,
not just the ones SlothDB happens to be fast on &mdash; the point of these
suites is to be honest about gaps.

## Reading the output

The `speedup` column is `DuckDB_time / SlothDB_time`. Values above 1.00x
mean SlothDB is faster on that query. The `Median speedup` is the median
of those ratios across all queries that ran on both engines; the
`Geomean speedup` is the geometric mean (which is what the SQL benchmarking
community usually reports for cross-query summaries).

`FAIL` in either column means the engine returned a non-zero exit code or
timed out. Failures don't count toward the summary.

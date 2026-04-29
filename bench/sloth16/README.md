# sloth16

A 16-query mixed suite covering the shapes most commonly hit in real
workloads: point counts, plain and filtered aggregates, GROUP BY, top-N,
nested aggregates (`ROUND(AVG(x))`, `SUM(x)/COUNT(*)`), and window
functions (`ROW_NUMBER`, `RANK`, `LAG`, `QUALIFY`).

## Run

Against the 10M-row Parquet:

```bash
python bench/run.py \
    --queries bench/sloth16/queries.sql \
    --table   data \
    --data    real-life-testing/sales_10m.parquet \
    --slothdb build/src/Release/slothdb.exe \
    --duckdb  real-life-testing/duckdb.exe \
    --runs    5 --warmup \
    --out     bench/sloth16/results_10m_parquet.md
```

Against the 10M-row CSV (slower &mdash; CSV parsing dominates):

```bash
python bench/run.py \
    --queries bench/sloth16/queries.sql \
    --table   data \
    --data    real-life-testing/sales_10m.csv \
    --slothdb build/src/Release/slothdb.exe \
    --duckdb  real-life-testing/duckdb.exe \
    --runs    3 --warmup \
    --out     bench/sloth16/results_10m_csv.md
```

The dataset has columns: `id, region, product, year, quantity, revenue, channel, ...`.
Q5 (`WHERE region = 'EU'`) returns 0 rows on the synthetic dataset
(regions are MEA/APAC/India/US-East/...). DuckDB skips it via parquet
min/max stats; SlothDB doesn't yet prune string equalities, so the time
gap there reflects a real (tracked) gap in string-stat pushdown rather
than a fundamental engine difference. Pick a region that exists in your
dataset for an apples-to-apples filter benchmark.

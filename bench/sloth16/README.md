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
Region values include APAC, EU-North, EU-South, India, LATAM, MEA, US-East,
US-West, Canada, Australia. Q5 filters on `region = 'APAC'` (about 1M of
the 10M rows). Edit the query if your dataset uses different values.

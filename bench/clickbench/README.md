# ClickBench

The 43-query analytical benchmark from
[ClickHouse/ClickBench](https://github.com/ClickHouse/ClickBench), verbatim
from their DuckDB variant (`_data/queries.sql`). Reproduced under Apache 2.0.

## Get the data

ClickBench uses a 100M-row `hits` table from the obfuscated Yandex.Metrica
weblog dataset. The single-file Parquet variant is ~14 GB:

```bash
curl -sSL https://datasets.clickhouse.com/hits_compatible/hits.parquet \
    -o ./hits.parquet
```

If you'd rather not pull 14 GB, grab the partitioned set (100 files, ~140 MB
each):

```bash
mkdir -p hits_parts
for i in $(seq 0 99); do
    curl -sSL "https://datasets.clickhouse.com/hits_compatible/partitioned/hits_$i.parquet" \
        -o "hits_parts/hits_$i.parquet"
done
```

Then point the runner at the glob: `--data 'hits_parts/*.parquet'`.

## Run

```bash
python bench/run.py \
    --queries bench/clickbench/queries.sql \
    --table   hits \
    --data    /path/to/hits.parquet \
    --slothdb build/src/Release/slothdb.exe \
    --duckdb  real-life-testing/duckdb.exe \
    --runs    3 --warmup \
    --out     bench/clickbench/results.md
```

## Known gaps as of 0.2.4

Some ClickBench queries use SQL features SlothDB doesn't fully cover yet.
The runner reports `FAIL` for those; they don't count toward the summary.
Expected gaps to be transparent about:

- `COUNT(DISTINCT col)` &mdash; the planner doesn't yet wire DISTINCT through
  the aggregate operator. Affects Q5, Q6, Q9, Q10, Q11, Q12, Q14, Q23.
- `extract(minute FROM EventTime)` and `DATE_TRUNC('minute', EventTime)`
  &mdash; partial support for date-part extraction. Affects Q19, Q43.
- `REGEXP_REPLACE` with capture groups &mdash; basic regex works but the
  back-reference rewrite path is partial. Affects Q29.
- `CASE WHEN ... THEN ... ELSE ... END` inside aggregates &mdash; affects Q40.

The other ~30 queries should run. File a Discord ping or a GitHub issue
if you hit a failure outside this list and we'll fix it &mdash; that's the
whole point of running ClickBench, surface the gaps so they get closed.

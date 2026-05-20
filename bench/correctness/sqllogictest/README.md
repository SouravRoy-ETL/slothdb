# SqlLogicTest

Drives Apache DataFusion's [`.slt` corpus](https://github.com/apache/datafusion/tree/main/datafusion/sqllogictest/test_files) through both SlothDB and DuckDB, in-process via their Python bindings. Reports per-file parity and speedup.

## Why parity with DuckDB and not against DataFusion's expected output

DataFusion's `.slt` files embed:

- DataFusion-specific physical-plan text under `query TT explain ...` blocks
- `set datafusion.execution.*` config statements
- DataFusion-specific output formatting (e.g. `(empty)`, NULL rendering)

Validating SlothDB against that expected output would conflate dialect differences with real correctness bugs. Instead the runner uses the `.slt` files as a query corpus, executes each block on both SlothDB and DuckDB, and compares the two outputs after sqllogictest-style normalisation. A query "passes" when SlothDB's normalised result equals DuckDB's. `EXPLAIN`, `DESCRIBE`, `SHOW`, and `set datafusion.*` statements are skipped.

## Setup

```bash
pip install slothdb duckdb
bash bench/correctness/sqllogictest/fetch_corpus.sh
```

`fetch_corpus.sh` sparse-clones the DataFusion repo to a tempdir and copies the 158 top-level `.slt` files (no subdirs yet) into `test_files/`. The corpus is gitignored — re-run the fetch script when DataFusion's tests change upstream.

## Run

```bash
python bench/correctness/sqllogictest/runner.py
python bench/correctness/sqllogictest/runner.py --file aggregate.slt
python bench/correctness/sqllogictest/runner.py --verbose
```

`--verbose` prints the first three diffs per file (SQL + SlothDB output + DuckDB output).

## Output columns

```
file                   parity/total   setup_ok    timing summary
```

- **parity / total** — queries where both engines ran and returned matching results / total queries attempted (after skipping EXPLAIN, dialect-specific config, and `query TT explain`)
- **setup_ok** — `CREATE TABLE` / `INSERT` / similar statements that succeeded on both engines / total setup statements
- **timing summary** — `n=<parity count> ratio(duck/sloth) median=<x> p10=<x> p90=<x> sloth_total=<ms> duck_total=<ms>`

A final `OVERALL` line aggregates across the run, plus a `SPEEDUP` line with the same shape.

## Current baseline (2026-05-20)

Top-level corpus only (158 `.slt` files, no subdirs).

```
OVERALL: parity 521/5806 (9.0%), disagree 188, setup_ok_both 465,
         setup_fail_sloth_only 316, setup_fail_duck_only 0
```

Progress from the first harness run on the same day:

```
initial    : parity 388/5806 (6.7%), disagree 197
after cast : parity 428/5806 (7.4%), disagree 161   (commit 034ff83)
after +    : parity 486/5806 (8.4%), disagree 161   (commit b0c26a7)
after IDF  : parity 488/5806 (8.4%), disagree 169   (commit 2b48cfc)
after subq : parity 521/5806 (9.0%), disagree 188   (commit dbda9e0)
```

Four engine fixes in the session: cast-to-narrow-int silently returning 0
(`ExecuteCast` missing TINYINT/SMALLINT/U\* cases), parser rejecting
unary `+`, IS [NOT] DISTINCT FROM not parsing, and subquery-in-FROM
(`SELECT * FROM (SELECT ...) AS s`) not parsing.

Cumulative: 388 → 521 parity (+133, +34%).

Reading the numbers honestly:

- **5221 of the 5806 queries errored on SlothDB but ran on DuckDB.** These are unsupported SQL features (arrays, window-function shapes, lateral joins, `UNNEST`, named arguments, custom types) — the big work item, not the speedup.
- **197 queries ran on SlothDB but disagreed with DuckDB.** These are correctness bugs to triage. `--verbose` surfaces the first few per file.
- **388 queries match.** On that subset SlothDB is median 5.6× faster, but the per-query times are sub-millisecond so the magnitude is noise-dominated. The ratio direction is informative; the absolute multiplier is not.
- **316 setup statements failed only on SlothDB.** Most are `CREATE EXTERNAL TABLE`, `CREATE FUNCTION`, or parquet/arrow loaders SlothDB doesn't ship.

Per-file results live in `results/baseline-2026-05-20.log`.

## Limitations

- Top-level `.slt` files only. The `array/`, `datetime/`, `pg_compat/`, `regexp/`, `spark/`, `string/`, `tpch/` subdirs aren't fetched yet.
- No per-query timeout. A pathological query that hangs SlothDB will hang the whole run; kill with Ctrl-C and add the file to a skip list.
- Result normalisation is intentionally lenient: NULL → `NULL`, ints stringified, reals to 3 decimals, text passes through. Type-letter mismatches between engines (DECIMAL vs DOUBLE on `2.5`) can produce false-disagree results. Tighten when the disagree count is interesting on its own merits.
- Both engines run in the same Python process via their bindings, on the same machine. Process startup, OS, and DuckDB version all bias the numbers; this isn't standardised-hardware territory.

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

Top-level corpus, 157/158 files (case.slt skipped — heap corruption in
slothdb cleanup after the file's full block sequence; documented in
`SKIP_FILES` in runner.py).

```
OVERALL: parity 650/5720 (11.4%), disagree 212, setup_ok_both 465,
         setup_fail_sloth_only 314, setup_fail_duck_only 0
```

Progress from the first harness run on the same day:

```
initial      : parity 388/5806 (6.7%), disagree 197
after cast   : parity 428/5806 (7.4%), disagree 161   (commit 034ff83)
after +      : parity 486/5806 (8.4%), disagree 161   (commit b0c26a7)
after IDF    : parity 488/5806 (8.4%), disagree 169   (commit 2b48cfc)
after subq   : parity 521/5806 (9.0%), disagree 188   (commit dbda9e0)
after union  : parity 533/5806 (9.2%), disagree 175   (commit 107bcd0 + 8281fa4)
after setop  : parity 532/5806 (9.2%), disagree 175   (commit 34fa08d)
after orderby: parity 532/5806 (9.2%), disagree 175   (commit e892438)
small mech   : parity 550/5806 (9.5%), disagree 183   (commit 3dcae85)
after VALUES : parity 595/5806 (10.2%), disagree 205  (commit 268e2c1)
case.slt skip: parity 572/5720 (10.0%), disagree 204  (commit bcf283a)
all/tstz     : parity 633/5720 (11.1%), disagree 209  (commit 8d28c25)
filter       : parity 650/5720 (11.4%), disagree 212  (commit e13b14e)
```

Twelve engine fixes in the session — every one a general SQL-standard
or engine-wide gap, not a corpus-specific patch:

1. Cast-to-narrow-int silently returning 0 — `ExecuteCast` missing
   TINYINT / SMALLINT / U\* cases.
2. Parser rejecting unary `+`.
3. `IS [NOT] DISTINCT FROM` not parsing.
4. Subquery-in-FROM (`SELECT * FROM (SELECT ...) AS s`) not parsing.
5. UNION ALL / UNION / INTERSECT / EXCEPT inside a CTE or subquery
   silently dropped everything past the LEFT side — derived-table
   materialisation only ran `Planner::Plan` on the bound LEFT side and
   the planner has no logical UNION node (set ops are walked at the
   Connection layer for top-level queries but the CTE / subquery paths
   weren't doing the same walk).
6. Set-op dedup used a string concatenation key (INT `1` collided with
   VARCHAR `'1'`, literal `NULL` collided with the string `'NULL'`) and
   no type widening across branches (BIGINT values silently truncated
   into INTEGER storage when LEFT was INTEGER). Now uses a Value-aware
   row hash with `Value::operator==`, plus a per-column common-type
   reduction across the chain with explicit coercion.
7. Top-level set-op handler only processed `sel.set_right` once, so
   `a UNION b UNION c` ran `a` vs `b` and silently dropped `c`. And
   `ORDER BY / LIMIT / OFFSET` placed after a UNION were attached by
   the parser to the rightmost leaf SelectStatement but per SQL standard
   they apply to the union as a whole. The rewrite walks the chain and
   re-applies leaf-stashed ORDER BY / LIMIT / OFFSET to the combined
   rows via a new `ApplyUnionOrderLimit` helper.
8. BOOLEAN equality / comparison threw `NotImplemented` — `CompareTyped`
   switch was missing the BOOL case (and the unsigned ints). Added,
   plus NULL guards in `left()` / `right()` and missing ISNAN / ISINF /
   ISFINITE / SINH-ATANH math families.
9. `VALUES (a,b), (c,d)` at top level and `FROM (VALUES ...) AS t(c1, c2)`
   in FROM both lower to a UNION-ALL chain of single-row SELECTs at
   parse time; the existing derived-table machinery then materialises
   them. Optional column-alias list applies real renames to the first
   SELECT's aliases.
10. `SELECT ALL`, `SUM(ALL col)`, `TIMESTAMPTZ` / `DATETIME` / `TIMESTAMP_S` /
    `TIMESTAMP_MS` / `TIMESTAMP_NS` type aliases — standard SQL syntax
    that other engines accept.
11. SQL:2003 `FILTER (WHERE ...)` on aggregates. Implemented via CASE-lift
    at the binder: `func(arg) FILTER (WHERE c)` becomes
    `func(CASE WHEN c THEN arg END)`. Rejects FILTER on non-aggregates
    with a clear bind-time error. Works for COUNT / SUM / AVG / MIN /
    MAX with GROUP BY.
12. **General engine fix uncovered by FILTER**: SUM / AVG / MIN / MAX /
    STDDEV / VARIANCE / MEDIAN / BOOL_* over a non-column-ref argument
    silently returned 0. `ComputeAggregates` only dispatched
    `col_idx != INVALID_INDEX` (direct column reads) and the COUNT
    branch; non-COUNT aggregates with an expression argument were
    falling through without updating state. Added a parallel
    `col_idx == INVALID_INDEX` branch that per-row-evaluates the
    expression via `ExpressionExecutor` and routes the value through
    the same per-name state updates. Benefits any query of shape
    `SUM/AVG(CASE WHEN ...)`, `SUM(a+b)`, `AVG(func(col))`, etc.

Cumulative: **388 → 650 parity (+262, +68%)** across twelve fixes.

Reading the numbers honestly:

- **~4858 of the 5720 queries errored on SlothDB but ran on DuckDB.** These are unsupported SQL features (arrays / structs / lateral joins / `UNNEST` / named arguments / custom types) — the long tail.
- **212 queries ran on SlothDB but disagreed with DuckDB.** Correctness gaps to triage. `--verbose` surfaces the first few per file.
- **650 queries match.** On that subset SlothDB is median ~5–6× faster, but per-query times are sub-millisecond so magnitude is noise-dominated.
- **314 setup statements failed only on SlothDB.** Mostly `CREATE EXTERNAL TABLE`, `CREATE FUNCTION`, parquet/arrow loaders.

Per-file results live in `results/`.

## Limitations

- Top-level `.slt` files only. The `array/`, `datetime/`, `pg_compat/`, `regexp/`, `spark/`, `string/`, `tpch/` subdirs aren't fetched yet.
- No per-query timeout. A pathological query that hangs SlothDB will hang the whole run; kill with Ctrl-C and add the file to a skip list.
- Result normalisation is intentionally lenient: NULL → `NULL`, ints stringified, reals to 3 decimals, text passes through. Type-letter mismatches between engines (DECIMAL vs DOUBLE on `2.5`) can produce false-disagree results. Tighten when the disagree count is interesting on its own merits.
- Both engines run in the same Python process via their bindings, on the same machine. Process startup, OS, and DuckDB version all bias the numbers; this isn't standardised-hardware territory.

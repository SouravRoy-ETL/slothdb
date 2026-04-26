# Iteration progress — 2026-04-26

Working from REPORT.md's gap analysis. Each row is one commit, with the
test-pass delta it produced.

| Commit | Change | Pass rate |
|---|---|---|
| baseline | 0.1.8 wheel as published | 108/170 (63.5%) |
| `e770661` | `::` postfix cast + `GROUP BY ALL` + new tokens (`[`, `]`, `{`, `}`, `::`, `:`) | 110/170 (64.7%) |
| `5411389` | `IF` / `IIF` / `IFNULL` / `NVL` + multi-arg `COALESCE` fix | **112/170 (65.9%)** |

Net: **+4 tests, +2.4 percentage points, ~2 hours of work**.

## What this taught me about realistic velocity

Each one-line gap on the report aggregates very different real costs:

- **Tier 4 fixes** (function aliases, single-token additions): 30 min each, +1 test each. Done a batch.
- **Tier 3 fixes** (FILTER clause, USING, list/struct literals): 2-4 hours each because they touch tokenizer + parser + AST + binder + executor + result types. **Not attempted yet.**
- **Tier 2 fixes** (date/time literals): 1-2 days because the type system has DATE/TIMESTAMP enum entries but no `Value::DATE()` factory, no string parsers, no arithmetic. **Discovered scope mid-session, parked.**
- **Tier 1 fixes** (top-N pushdown, HTTP Range, binder `__FILE__` recursion): 1-3 days each — planner, reader, or AST-walker rewrites. **Not attempted yet.**

Net point: **single sessions move the pass rate by 2-3 points**. Hitting 90% is 8-10 more sessions, not one.

## Phased plan (what to do next, in order)

### Session 2 — binder `__FILE__` bug (highest leverage)
Single root cause behind ~10 tests across subqueries, CTEs, EXPLAIN,
and complex queries.

The fix path is clear:
1. Refactor the file-rewriting logic in `connection.cpp:1385-1485` into
   a standalone helper `MaterializeFileRef(TableRef&, Catalog&, ...)`.
2. Add an AST walker that visits every nested SELECT (CTEs +
   `SubqueryExpression` nodes inside select_list / where / having /
   qualify).
3. Apply the helper to every `TableRef` found.

Expected gain: **+8-10 tests** (subq×7, cte×3, intro×2 partial).

### Session 3 — DATE / TIMESTAMP first-class values
Currently DATE/TIMESTAMP are LogicalType enum entries with no `Value::`
factory. Need:
1. `Value::DATE(int32 days_since_epoch)` and `Value::TIMESTAMP(int64 us_since_epoch)`.
2. String parsers `ParseDate("YYYY-MM-DD")` and `ParseTimestamp("YYYY-MM-DD HH:MM:SS")`.
3. Parser: `DATE 'literal'` and `TIMESTAMP 'literal'` typed-literal handling.
4. Wire to existing EXTRACT, DATE_DIFF, STRFTIME, INTERVAL arithmetic.

Expected gain: **+6-8 tests** (dates×8, plus unblocks `complex/moving_avg` etc).

### Session 4 — Top-N pushdown for `ORDER BY ... LIMIT N`
Single biggest perf bug (6,154x slower on 10M-row Parquet). Path:
1. Detect ORDER BY + LIMIT in the planner.
2. Emit a bounded-heap operator instead of full sort + truncate.

Expected gain: **6-10x speedup on a query everyone types**.
No coverage tests blocked, but fixes the worst regression in the report.

### Session 5 — Parquet predicate pushdown via row-group statistics
75x slowdown on `WHERE quantity > 50` over 10M-row Parquet. Path:
1. Read `min/max` per column per row group from the Parquet metadata.
2. In the scan operator, check predicates against the stats and skip
   row groups that can't satisfy them.

Expected gain: **20-50x on selective filters**. No coverage tests, just perf.

### Session 6 — HTTP Range requests for Parquet
22x slowdown over HTTPS (132s vs 6s). Path:
1. The `HTTPClient` should issue a Range request for the last 64 KB
   first to read the Parquet footer.
2. Parse footer to get column-chunk byte offsets.
3. Range-fetch only the columns needed by the query.

Expected gain: **10-30x speedup on remote Parquet**.
Real users care about this for object-storage workflows.

### Session 7 — `JOIN ... USING(col)` + `SEMI JOIN` / `ANTI JOIN`
Three tests blocked, plus DuckDB-compat polish. Path:
1. Add `KW_SEMI`, `KW_ANTI` tokens.
2. Recognise them as join types in the parser.
3. Convert `USING(col)` to an equivalent ON clause at parse time.

Expected gain: **+3-5 tests**.

### Session 8 — `count(*) FILTER (WHERE ...)`
1 test blocked. Filter-aware aggregate is a parser + planner change.

Expected gain: **+1-2 tests**, but it's the kind of feature analysts
hit constantly.

### Session 9 — `VALUES` as a table source
1 test blocked. `FROM (VALUES (1,'a'),(2,'b'))` should bind as an
inline table.

Expected gain: **+1-2 tests**, but it unblocks every "let me inline
some test data" use case.

### Session 10+ — list/struct/JSON first-class types
This is multi-week work and probably should be its own milestone.
Tokens already added in session 1; everything downstream (AST node
types, binder rules, executor paths, result serialization,
type system extensions) is still missing.

Expected gain: **+14 tests across list/struct/json**, plus opens up
JSON pathing which is heavily used in real OLAP.

## Things this run validated

- The harness (`harness.py`) correctly catches regressions. Every fix
  added has been verified to not break previously-passing tests.
- The `:: cast`, `GROUP BY ALL`, `IF/IIF/IFNULL/NVL` fixes all
  work on real queries beyond just the test cases (smoke-tested in
  the Python wheel before re-running the suite).
- The `__FILE__` binder bug is more pervasive than I first thought —
  it blocks `EXPLAIN`, almost every `complex` test, and arguably is
  the single highest-leverage code change in the queue.

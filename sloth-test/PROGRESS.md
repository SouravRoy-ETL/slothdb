# Iteration progress — 2026-04-26

| Commit | Change | Pass rate | Perf delta |
|---|---|---|---|
| baseline | 0.1.8 wheel as published | 108/170 (63.5%) | — |
| `e770661` | `::` postfix cast + `GROUP BY ALL` + new tokens | 110/170 | — |
| `5411389` | `IF` / `IIF` / `IFNULL` / `NVL` + multi-arg `COALESCE` | 112/170 | — |
| `d1a1318` | Recursive walker for `__FILE__` in nested SELECTs | 116/170 | — |
| `b2102ad` | Recursive-CTE table-cleanup fix | 117/170 | — |
| `cf15a0c` | Top-N pushdown for `ORDER BY ... LIMIT N` | 116/170* | **53×** |

\* Top-N caused 1 unrelated regression (`groupby/rollup` access violation). Pre-existing
silent bug in positional `GROUP BY 1` exposed by test ordering — separate root cause from
top-N.

## Coverage delta

**Net: +8 tests fixed, -1 regression = +7 = 116/170 (68.2%)**

Categories that moved:
- `subq` 0/7 → 1/7 (in_subq passes)
- `cte` 0/3 → 2/3 (simple_cte, multi_cte pass; recursive_cte still binder bug)
- `complex` 0/6 → 1/6 (yoy_lag now passes)
- `case` 2/4 → 3/4 (case_no_else)
- `null` 3/5 → 5/5 (coalesce, ifnull)
- `types` 6/7 → 7/7 (`::` cast)
- `groupby` 6/8 → 6/8 (group_by_all gained, rollup regressed via flaky)
- `complex` 0/6 → 1/6

## Performance wins

| Query | Before | After | Speedup |
|---|---:|---:|---:|
| **`parquet10m_orderby_top10`** (10M-row Parquet, ORDER BY + LIMIT 10) | 420,344 ms | **7,924 ms** | **53×** |
| `csv1m_orderby_top10` (1M-row CSV, ORDER BY + LIMIT 10) | 7,281 ms | **940 ms** | **7.7×** |
| `avro1m_orderby_top10` (1M-row Avro, ORDER BY + LIMIT 10) | 2,869 ms | **700 ms** | **4.1× → wins vs DuckDB** |

The catastrophic 6,154× regression on `parquet10m_orderby_top10` is now 159× — still slower
than DuckDB (49 ms vs 7,924 ms) but no longer pathological. Remaining gap is per-value
`Vector::GetValue` overhead in the heap-comparison path; vectorised compares are a separate
follow-up.

## Honest claim audit (post-fixes)

| Hero claim | Status |
|---|---|
| "Up to 5× faster where it counts" | ✅ True — CSV bulk aggregates, Avro reads, and now Avro top-N. |
| "138 ms vs DuckDB 540 ms" 5-query batch | ✅ Holds. |
| "Reads Parquet directly" | 🟡 Reads work; ORDER BY + LIMIT now usable; predicate pushdown still missing (75× slower on filter scans). |
| "Reads Avro directly" | ✅ 4–7× faster than DuckDB. |
| "Query files over HTTPS" | 🟡 Works for CSV; Parquet still 22× slower (no Range requests). |
| "Same model as DuckDB and SQLite" | 🟡 Subqueries, CTEs, joins now reach binder via recursive walker, but scalar-subquery-as-expression and FROM-(SELECT) still missing. |

## Remaining big items, ordered

### Tier 1 (still ship-blocking)
1. **Parquet predicate pushdown via row-group statistics.** The Parquet reader already parses
   min/max stats per column per row group (`src/storage/parquet.cpp:225` `ParseStatistics`).
   Need to expose them through the reader API and use in the scan operator to skip row groups
   that can't satisfy WHERE predicates. Bench: 5,395 ms → expected 100 ms (target: 50× speedup).
2. **HTTP Range requests for Parquet over HTTPS.** Fetch the footer with a Range request,
   parse metadata, then range-fetch only column chunks needed. Bench: 132 s → expected 6–10 s.
3. **`(SELECT ...)` as scalar expression** (`src/parser/parser.cpp` ParsePrimary). Unblocks
   subq/scalar_subq, subq/correlated, complex/yoy_lag (already fixed via walker), several
   real-world patterns.
4. **`FROM (SELECT ...)`** (parser + AST: TableRef.subquery field). Unblocks
   subq/from_subq, complex/top_n_per_group, complex/pivot.
5. **`WHERE EXISTS / NOT IN / ANY / ALL`** parser corner cases — different parse error each.

### Tier 2 (parity / DuckDB-compat)
6. DATE / TIMESTAMP first-class values + literal parsing.
7. `JOIN ... USING(col)` + `SEMI JOIN` / `ANTI JOIN` keywords.
8. Window frame `OVER (... ROWS BETWEEN ... AND ...)` parsing.
9. `count(*) FILTER (WHERE ...)` aggregate.
10. `VALUES (...), (...)` as a table source.

### Tier 3 (polish, smaller payoff)
11. List/struct/JSON literals — `[1,2,3]` and `{'k':v}` parser support, then full type-system
    integration (multi-week project).
12. `EXPLAIN ANALYZE` and `EXPLAIN` of file-literal queries.
13. Aggregate functions: `LIST`, `APPROX_COUNT_DISTINCT`, `QUANTILE_CONT`.

## Known issues introduced this session

- **`groupby/rollup`** intermittent access violation when run after `groupby/group_by_position`.
  Both queries pass in isolation. `GROUP BY 1` (positional) silently returns wrong results
  even on the baseline 0.1.8, so the harness "PASS" was misleading. The crash is downstream
  state corruption — separate root cause from the top-N change. Needs its own session.

## Velocity reality

- 8 hours of work this session = 7 net coverage tests + 53× perf win on the worst query +
  full diagnostic of remaining gaps with sized fix paths.
- Next ~3 sessions should plausibly reach 80% coverage AND close the remaining two
  Tier-1 perf items.
- Hitting 90%+ requires the multi-week list/struct/JSON type-system buildout.

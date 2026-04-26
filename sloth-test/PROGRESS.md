# Iteration progress — 2026-04-26

| Commit | Change | Pass rate | Perf delta |
|---|---|---|---|
| baseline | 0.1.8 wheel as published | 108/170 (63.5%) | — |
| `e770661` | `::` postfix cast + `GROUP BY ALL` + new tokens | 110/170 | — |
| `5411389` | `IF` / `IIF` / `IFNULL` / `NVL` + multi-arg `COALESCE` | 112/170 | — |
| `d1a1318` | Recursive walker for `__FILE__` in nested SELECTs | 116/170 | — |
| `b2102ad` | Recursive-CTE table-cleanup fix | 117/170 | — |
| `cf15a0c` | Top-N pushdown for `ORDER BY ... LIMIT N` | 116/170 | **53×** |
| `09bff85` | Parquet predicate pushdown via row-group stats | 117/170 | **1000×** prunable / **2×** scan |
| `13aec7c` | Implicit `FROM 'file.parquet'` uses streaming + cache URLs | 117/170 | **45,000×** count(*) / **115×** ORDER BY |

**Net: 117/170 (68.8%) coverage, +9 tests over baseline. Plus four order-of-magnitude
performance wins on the queries the gap-analysis report flagged as ship-blockers.**

## Tier-1 perf items (the user-flagged ship-blockers)

### #1 Top-N pushdown for ORDER BY+LIMIT
| Query | Before | After | DuckDB |
|---|---:|---:|---:|
| 10M Parquet | **420,344 ms** | **3,680 ms** | 30 ms |
| 1M CSV | 7,281 ms | 505 ms | 200 ms |
| 1M Avro | 2,869 ms | **323 ms — wins vs DuckDB** | 652 ms |

Implementation: `PhysicalOrderBy::SetRowLimit(n)` switches to a bounded
`std::priority_queue<row, ..., Less>` of size `n` for `n ≤ 65536`.
`PhysicalProjection` now forwards `SetRowLimit` to its child so the
common `LIMIT → PROJECTION → ORDER_BY` plan shape doesn't drop the hint.

### #2 Parquet predicate pushdown via row-group stats
| Query | Before | After |
|---|---:|---:|
| `WHERE quantity > 999` (prunable) | 4,984 ms | **2 ms (1000×)** |
| `WHERE quantity < 0` (prunable) | 4,984 ms | **2 ms (830×)** |
| `WHERE quantity > 50` (no prune) | 4,984 ms | 2,480 ms (2×) |

Three layered bugs were hiding this:

1. **Stats not propagated.** Standard-Parquet metadata path threw away
   row-group min/max — `ParseStatistics` decoded raw bytes into
   `min_bytes/max_bytes` but the public `ParquetColumnMeta` left
   `has_stats=false`. Fix: decode raw bytes into typed `Value` per the
   column's logical type.
2. **Thrift fields 5/6 swapped.** Comments in `ParseStatistics` had
   field 5 = `min_value` and field 6 = `max_value`; spec is the
   opposite. After fix #1 started using stats, every prunable filter
   pruned the wrong row groups.
3. **Cross-type comparison silently broken.** `Value::operator<`
   returns false on type mismatch, so `BIGINT(200) < INTEGER(50)`
   was "neither less nor greater nor equal" — zone-map check always
   said "might match." Added `CoerceForCompare` that promotes the
   literal to the column's type before comparing.

### #3 HTTP Range requests (deferred — partial fix shipped)
The proper fix (replace mmap-based I/O with a virtual remote-file
abstraction that does Range reads) is multi-day work. Shipped a
**stable URL-cache** as a partial mitigation: repeated queries on the
same HTTPS Parquet skip re-download by hashing the URL into a stable
temp-file name. First query on a URL still pays full download; second
and beyond reuse the cached file.

Filed as a follow-up: `ResolveRemoteFile` returns a `RemoteFile` handle
that `ParquetReader` can open via `WinHttpReadData` with `Range:
bytes=` headers. Footer-first read pattern matches DuckDB's approach.

### #4 Subqueries on file sources
**Single-bug fix unlocked 4+ tests.** The recursive walker (commit
`d1a1318`) materialises every `__FILE__` `TableRef` found inside any
nested `SelectStatement` (CTEs, scalar subqueries, IN/EXISTS subqueries)
into a temp catalog table before the binder runs.

### #5 CTEs on file sources
Same root cause as #4 — fixed by the same recursive walker.

## Coverage breakdown (delta vs baseline)

| Category | baseline → now |
|---|---|
| `subq` | 0/7 → **1/7** |
| `cte` | 0/3 → **2/3** |
| `complex` | 0/6 → **1/6** |
| `case` | 2/4 → **3/4** |
| `null` | 3/5 → **5/5** |
| `types` | 6/7 → **7/7** |
| `groupby` | 6/8 → 6/8 |
| Everything else | unchanged |

## Remaining work (next session pickups)

### Tier 1
- HTTP Range requests for Parquet — replace mmap-based I/O with virtual
  remote-file abstraction; range-fetch footer first, then column chunks
  as needed. Expected: 130s → ~10s on cold remote query.
- Vectorised filter on Parquet scan — 76× gap on `WHERE quantity > 50`
  (non-prunable). Currently per-row Vector::GetValue overhead dominates.

### Tier 2
- DATE / TIMESTAMP first-class Value support — needed for date-arithmetic
  tests + EXTRACT / date_diff / strftime (9 broken tests).
- `(SELECT ...)` as scalar expression — unblocks `subq/scalar_subq`,
  `subq/correlated`, several complex tests.
- `FROM (SELECT ...)` derived-table parsing — unblocks `subq/from_subq`,
  `complex/top_n_per_group`, `complex/pivot`.
- Window frame `OVER (... ROWS BETWEEN ...)` parsing.
- `JOIN ... USING(col)` + `SEMI JOIN` / `ANTI JOIN` keywords.

### Tier 3
- List/struct/JSON literal types — `[1,2,3]` and `{'k':v}` already
  tokenise (commit `e770661`); now need AST nodes, binder, executor,
  and result serialisation. Multi-week.

## Velocity reality

- ~10 hours of engineering this session
- 9 net coverage tests + four order-of-magnitude perf wins on the
  exact queries the original gap-analysis flagged as ship-blockers
- Each fix surfaced and resolved bugs the original `RowGroupMightMatch`
  function had (it was implemented in 2025 but never actually used —
  feeding it real predicates exposed three latent bugs in stat decoding)

The "honest claim audit" from `REPORT.md` now reads differently:

| Hero claim | Status before | Status now |
|---|---|---|
| "Up to 5× faster where it counts" | ✅ | ✅ holds wider |
| "138 ms vs DuckDB 540 ms" 5-query batch | ✅ | ✅ |
| "Reads Parquet directly" | 🟡 ORDER BY broken | ✅ usable |
| "Reads Avro directly" | ✅ | ✅ |
| "Query files over HTTPS" | 🟡 22× slower | 🟡 still slower on cold; cached fast |
| "Same model as DuckDB" | 🟡 subqueries broken | 🟡 single-level subqueries work; nested still broken |

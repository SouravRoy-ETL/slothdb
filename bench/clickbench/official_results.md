# ClickBench-43: SlothDB vs DuckDB

43 official ClickBench queries (DuckDB variant, verbatim from
ClickHouse/ClickBench). Both engines query hits_typed.parquet directly on
the same machine; 3 trials per engine per query, min-of-3 reported;
120s per-trial timeout. SlothDB local build vs DuckDB v1.4.3. This is
the official query set and 3-run methodology, run locally; it is not an
official ClickBench submission (that uses standardised cloud hardware).

| Q | SlothDB | DuckDB | DuckDB/SlothDB | result |
|--:|--:|--:|--:|:--|
| 1 | 135 ms | 280 ms | 2.08x | match |
| 2 | 180 ms | 328 ms | 1.82x | match |
| 3 | 245 ms | 422 ms | 1.73x | match |
| 4 | 314 ms | 419 ms | 1.33x | match |
| 5 | 998 ms | 935 ms | 0.94x | match |
| 6 | 987 ms | 1424 ms | 1.44x | match |
| 7 | 98 ms | 296 ms | 3.01x | match |
| 8 | 160 ms | 250 ms | 1.56x | match |
| 9 | 1320 ms | 1139 ms | 0.86x | match |
| 10 | 1573 ms | 1356 ms | 0.86x | match |
| 11 | 455 ms | 480 ms | 1.05x | match |
| 12 | 465 ms | 523 ms | 1.13x | match |
| 13 | 944 ms | 1476 ms | 1.56x | match |
| 14 | 3903 ms | 2963 ms | 0.76x | match |
| 15 | 1313 ms | 1723 ms | 1.31x | match |
| 16 | 1004 ms | 1053 ms | 1.05x | match |
| 17 | 3671 ms | 3821 ms | 1.04x | match |
| 18 | 177 ms | 2616 ms | 14.81x | DIFF |
| 19 | TIMEOUT | 9030 ms |  | sloth-only-fail (TIMEOUT) |
| 20 | 228 ms | 353 ms | 1.55x | match |
| 21 | 2515 ms | 2479 ms | 0.99x | match |
| 22 | 2660 ms | 2628 ms | 0.99x | match |
| 23 | 6260 ms | 4060 ms | 0.65x | match |
| 24 | 635 ms | 1024 ms | 1.61x | match |
| 25 | 172 ms | 506 ms | 2.94x | match |
| 26 | 464 ms | 666 ms | 1.44x | match |
| 27 | 172 ms | 336 ms | 1.95x | match |
| 28 | 2270 ms | 2298 ms | 1.01x | match |
| 29 | TIMEOUT | 18654 ms |  | sloth-only-fail (TIMEOUT) |
| 30 | 166 ms | 323 ms | 1.95x | match |
| 31 | 5469 ms | 1229 ms | 0.22x | DIFF |
| 32 | 15187 ms | 1602 ms | 0.11x | DIFF |
| 33 | TIMEOUT | 10045 ms |  | sloth-only-fail (TIMEOUT) |
| 34 | 6544 ms | 11329 ms | 1.73x | match |
| 35 | 6338 ms | 11728 ms | 1.85x | match |
| 36 | 959 ms | 1219 ms | 1.27x | match |
| 37 | 179 ms | 287 ms | 1.60x | match |
| 38 | 113 ms | 258 ms | 2.29x | match |
| 39 | 147 ms | 267 ms | 1.82x | DIFF |
| 40 | 1358 ms | 368 ms | 0.27x | DIFF |
| 41 | 137 ms | 253 ms | 1.85x | DIFF |
| 42 | 122 ms | 256 ms | 2.10x | DIFF |
| 43 | 1015 ms | 249 ms | 0.25x | match |

**Comparable queries (both engines ran): 40 of 43.**

- SlothDB faster on 29 of 40; DuckDB faster on 11.
- Geomean DuckDB/SlothDB time ratio: 1.24x (above 1.0 means SlothDB faster on average).
- Total min-of-3 wall time: SlothDB 71.1s, DuckDB 65.2s.
- Results vs DuckDB: 33 matched, 7 differed (need review), 3 not comparable (an engine errored or timed out).

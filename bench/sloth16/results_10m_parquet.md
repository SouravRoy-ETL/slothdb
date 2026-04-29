# Benchmark results

- Queries: `bench/sloth16/queries.sql`
- Data: `real-life-testing/sales_10m.parquet`
- Runs per query: 3 (with warmup)

| # | SlothDB | DuckDB | Speedup | Query |
|--:|--:|--:|:-:|---|
| 1 | 10 ms | 32 ms | 3.25x | `SELECT COUNT(*) FROM data` |
| 2 | 85 ms | 67 ms | 0.79x | `SELECT SUM(revenue) FROM data` |
| 3 | 105 ms | 86 ms | 0.82x | `SELECT MIN(revenue), MAX(revenue), AVG(revenue) FROM data` |
| 4 | 49 ms | 84 ms | 1.70x | `SELECT COUNT(*) FROM data WHERE year >= 2023` |
| 5 | 1.23 s | 35 ms | 0.03x | `SELECT SUM(revenue) FROM data WHERE region = 'EU'` |
| 6 | 131 ms | 114 ms | 0.87x | `SELECT region, COUNT(*), SUM(revenue) FROM data GROUP BY region ORDER BY region` |
| 7 | 206 ms | 198 ms | 0.96x | `SELECT product, year, SUM(revenue) FROM data GROUP BY product, year ORDER BY SUM(revenu...` |
| 8 | 287 ms | 287 ms | 1.00x | `SELECT region, channel, COUNT(*), SUM(revenue) FROM data WHERE year >= 2023 AND quantit...` |
| 9 | 134 ms | 111 ms | 0.83x | `SELECT product, SUM(revenue) AS total FROM data GROUP BY product ORDER BY total DESC LI...` |
| 10 | 110 ms | 111 ms | 1.00x | `SELECT id, revenue FROM data ORDER BY revenue DESC LIMIT 10` |
| 11 | 133 ms | 114 ms | 0.86x | `SELECT region, ROUND(AVG(revenue)) AS avg_r FROM data GROUP BY region ORDER BY avg_r DESC` |
| 12 | 140 ms | 115 ms | 0.82x | `SELECT region, SUM(revenue) / COUNT(*) AS per_row FROM data GROUP BY region ORDER BY pe...` |
| 13 | 1.78 s | 2.55 s | 1.43x | `SELECT region, id, ROW_NUMBER() OVER (PARTITION BY region ORDER BY revenue DESC) AS rn ...` |
| 14 | 1.82 s | 2.42 s | 1.33x | `SELECT product, revenue, RANK() OVER (PARTITION BY product ORDER BY revenue DESC) AS r ...` |
| 15 | 1.33 s | 2.30 s | 1.73x | `SELECT region, year, revenue, LAG(revenue, 1) OVER (PARTITION BY region ORDER BY year) ...` |
| 16 | 1.28 s | 2.70 s | 2.11x | `SELECT region, id, revenue FROM data QUALIFY ROW_NUMBER() OVER (PARTITION BY region ORD...` |

**16/16 queries ran on both. SlothDB faster on 8/16. Median speedup: 0.98x.**

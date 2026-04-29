# Benchmark results

- Queries: `bench/sloth16/queries.sql`
- Data: `real-life-testing/sales_10m.parquet`
- Runs per query: 5 (with warmup)

| # | SlothDB | DuckDB | Speedup | Query |
|--:|--:|--:|:-:|---|
| 1 | 12 ms | 38 ms | 3.21x | `SELECT COUNT(*) FROM data` |
| 2 | 94 ms | 74 ms | 0.79x | `SELECT SUM(revenue) FROM data` |
| 3 | 110 ms | 95 ms | 0.86x | `SELECT MIN(revenue), MAX(revenue), AVG(revenue) FROM data` |
| 4 | 55 ms | 89 ms | 1.64x | `SELECT COUNT(*) FROM data WHERE year >= 2023` |
| 5 | 1.44 s | 115 ms | 0.08x | `SELECT SUM(revenue) FROM data WHERE region = 'APAC'` |
| 6 | 146 ms | 119 ms | 0.82x | `SELECT region, COUNT(*), SUM(revenue) FROM data GROUP BY region ORDER BY region` |
| 7 | 230 ms | 208 ms | 0.90x | `SELECT product, year, SUM(revenue) FROM data GROUP BY product, year ORDER BY SUM(revenu...` |
| 8 | 310 ms | 308 ms | 1.00x | `SELECT region, channel, COUNT(*), SUM(revenue) FROM data WHERE year >= 2023 AND quantit...` |
| 9 | 144 ms | 121 ms | 0.84x | `SELECT product, SUM(revenue) AS total FROM data GROUP BY product ORDER BY total DESC LI...` |
| 10 | 128 ms | 120 ms | 0.93x | `SELECT id, revenue FROM data ORDER BY revenue DESC LIMIT 10` |
| 11 | 145 ms | 121 ms | 0.83x | `SELECT region, ROUND(AVG(revenue)) AS avg_r FROM data GROUP BY region ORDER BY avg_r DESC` |
| 12 | 154 ms | 128 ms | 0.83x | `SELECT region, SUM(revenue) / COUNT(*) AS per_row FROM data GROUP BY region ORDER BY pe...` |
| 13 | 2.05 s | 2.78 s | 1.35x | `SELECT region, id, ROW_NUMBER() OVER (PARTITION BY region ORDER BY revenue DESC) AS rn ...` |
| 14 | 2.03 s | 2.74 s | 1.35x | `SELECT product, revenue, RANK() OVER (PARTITION BY product ORDER BY revenue DESC) AS r ...` |
| 15 | 1.50 s | 2.61 s | 1.74x | `SELECT region, year, revenue, LAG(revenue, 1) OVER (PARTITION BY region ORDER BY year) ...` |
| 16 | 1.45 s | 2.87 s | 1.98x | `SELECT region, id, revenue FROM data QUALIFY ROW_NUMBER() OVER (PARTITION BY region ORD...` |

**16/16 queries ran on both. SlothDB faster on 6/16. Median speedup: 0.92x.**

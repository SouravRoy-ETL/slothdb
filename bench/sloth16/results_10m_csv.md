# Benchmark results

- Queries: `bench/sloth16/queries.sql`
- Data: `real-life-testing/sales_10m.csv`
- Runs per query: 3 (with warmup)

| # | SlothDB | DuckDB | Speedup | Query |
|--:|--:|--:|:-:|---|
| 1 | 159 ms | 805 ms | 5.07x | `SELECT COUNT(*) FROM data` |
| 2 | 771 ms | 825 ms | 1.07x | `SELECT SUM(revenue) FROM data` |
| 3 | 819 ms | 865 ms | 1.06x | `SELECT MIN(revenue), MAX(revenue), AVG(revenue) FROM data` |
| 4 | 2.05 s | 812 ms | 0.40x | `SELECT COUNT(*) FROM data WHERE year >= 2023` |
| 5 | 2.55 s | 865 ms | 0.34x | `SELECT SUM(revenue) FROM data WHERE region = 'EU'` |
| 6 | 944 ms | 924 ms | 0.98x | `SELECT region, COUNT(*), SUM(revenue) FROM data GROUP BY region ORDER BY region` |
| 7 | 1.09 s | 961 ms | 0.89x | `SELECT product, year, SUM(revenue) FROM data GROUP BY product, year ORDER BY SUM(revenu...` |
| 8 | 1.05 s | 1.02 s | 0.97x | `SELECT region, channel, COUNT(*), SUM(revenue) FROM data WHERE year >= 2023 AND quantit...` |
| 9 | 933 ms | 911 ms | 0.98x | `SELECT product, SUM(revenue) AS total FROM data GROUP BY product ORDER BY total DESC LI...` |
| 10 | 1.87 s | 895 ms | 0.48x | `SELECT id, revenue FROM data ORDER BY revenue DESC LIMIT 10` |
| 11 | 959 ms | 934 ms | 0.97x | `SELECT region, ROUND(AVG(revenue)) AS avg_r FROM data GROUP BY region ORDER BY avg_r DESC` |
| 12 | 1.05 s | 991 ms | 0.95x | `SELECT region, SUM(revenue) / COUNT(*) AS per_row FROM data GROUP BY region ORDER BY pe...` |
| 13 | 2.13 s | 3.31 s | 1.55x | `SELECT region, id, ROW_NUMBER() OVER (PARTITION BY region ORDER BY revenue DESC) AS rn ...` |
| 14 | 2.27 s | 3.53 s | 1.55x | `SELECT product, revenue, RANK() OVER (PARTITION BY product ORDER BY revenue DESC) AS r ...` |
| 15 | 1.78 s | 3.18 s | 1.79x | `SELECT region, year, revenue, LAG(revenue, 1) OVER (PARTITION BY region ORDER BY year) ...` |
| 16 | 1.71 s | 3.49 s | 2.04x | `SELECT region, id, revenue FROM data QUALIFY ROW_NUMBER() OVER (PARTITION BY region ORD...` |

**16/16 queries ran on both. SlothDB faster on 7/16. Median speedup: 0.98x.**

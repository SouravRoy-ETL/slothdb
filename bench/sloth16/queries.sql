-- SlothDB 16-query suite. Mixed shapes (point counts, GROUP BY, filtered
-- aggregates, top-N, window functions) over the same input file. The
-- runner substitutes the bare identifier `data` with a quoted path:
--   python bench/run.py --queries bench/sloth16/queries.sql \
--       --table data --data real-life-testing/sales_10m.csv \
--       --slothdb build/src/Release/slothdb.exe \
--       --duckdb real-life-testing/duckdb.exe \
--       --runs 5 --warmup --out bench/sloth16/results.md

-- Block A: point queries
SELECT COUNT(*) FROM data;
SELECT SUM(revenue) FROM data;
SELECT MIN(revenue), MAX(revenue), AVG(revenue) FROM data;

-- Block B: filtered point queries
SELECT COUNT(*) FROM data WHERE year >= 2023;
SELECT SUM(revenue) FROM data WHERE region = 'EU';

-- Block C: simple GROUP BY
SELECT region, COUNT(*), SUM(revenue) FROM data GROUP BY region ORDER BY region;
SELECT product, year, SUM(revenue) FROM data GROUP BY product, year ORDER BY SUM(revenue) DESC LIMIT 10;
SELECT region, channel, COUNT(*), SUM(revenue) FROM data WHERE year >= 2023 AND quantity > 100 GROUP BY region, channel ORDER BY SUM(revenue) DESC;

-- Block D: top-N + ORDER BY
SELECT product, SUM(revenue) AS total FROM data GROUP BY product ORDER BY total DESC LIMIT 10;
SELECT id, revenue FROM data ORDER BY revenue DESC LIMIT 10;

-- Block E: nested aggregates (regression coverage for the ROUND(AVG()) fix)
SELECT region, ROUND(AVG(revenue)) AS avg_r FROM data GROUP BY region ORDER BY avg_r DESC;
SELECT region, SUM(revenue) / COUNT(*) AS per_row FROM data GROUP BY region ORDER BY per_row DESC;

-- Block F: window functions
SELECT region, id, ROW_NUMBER() OVER (PARTITION BY region ORDER BY revenue DESC) AS rn FROM data LIMIT 10;
SELECT product, revenue, RANK() OVER (PARTITION BY product ORDER BY revenue DESC) AS r FROM data LIMIT 10;
SELECT region, year, revenue, LAG(revenue, 1) OVER (PARTITION BY region ORDER BY year) AS prev FROM data LIMIT 10;
SELECT region, id, revenue FROM data QUALIFY ROW_NUMBER() OVER (PARTITION BY region ORDER BY revenue DESC) = 1 LIMIT 10;

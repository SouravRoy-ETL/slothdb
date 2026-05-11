# Benchmark results

- Queries: `bench/clickbench/queries.sql`
- Data: `bench/clickbench/data/hits.parquet`
- Runs per query: 1 (with warmup)

| # | SlothDB | DuckDB | Speedup | Query |
|--:|--:|--:|:-:|---|
| 1 | 33 ms | 105 ms | 3.20x | `SELECT COUNT(*) FROM hits` |
| 2 | 75 ms | 167 ms | 2.23x | `SELECT COUNT(*) FROM hits WHERE AdvEngineID <> 0` |
| 3 | 184 ms | 230 ms | 1.25x | `SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM hits` |
| 4 | 163 ms | 232 ms | 1.42x | `SELECT AVG(UserID) FROM hits` |
| 5 | 1.01 s | 1.04 s | 1.03x | `SELECT COUNT(DISTINCT UserID) FROM hits` |
| 6 | 1.83 s | 1.30 s | 0.71x | `SELECT COUNT(DISTINCT SearchPhrase) FROM hits` |
| 7 | 34 ms | 184 ms | 5.43x | `SELECT MIN(EventDate), MAX(EventDate) FROM hits` |
| 8 | 112 ms | 177 ms | 1.58x | `SELECT AdvEngineID, COUNT(*) FROM hits WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDE...` |
| 9 | 1.24 s | 1.29 s | 1.03x | `SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM hits GROUP BY RegionID ORDER BY u DES...` |
| 10 | 1.95 s | 1.66 s | 0.85x | `SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT ...` |
| 11 | 486 ms | 444 ms | 0.91x | `SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <...` |
| 12 | 543 ms | 515 ms | 0.95x | `SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE Mobil...` |
| 13 | 2.04 s | 1.47 s | 0.72x | `SELECT SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPh...` |
| 14 | FAIL | 2.62 s |  | `SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM hits WHERE SearchPhrase <> '' GRO...` |
| 15 | 1.97 s | 1.70 s | 0.86x | `SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' G...` |
| 16 | 1.01 s | 1.21 s | 1.20x | `SELECT UserID, COUNT(*) FROM hits GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10` |
| 17 | 4.26 s | 3.14 s | 0.74x | `SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase ORDER BY ...` |
| 18 | 4.21 s | 2.48 s | 0.59x | `SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase LIMIT 10` |
| 19 | FAIL | FAIL |  | `SELECT UserID, extract(minute FROM EventTime) AS m, SearchPhrase, COUNT(*) FROM hits GR...` |
| 20 | 285 ms | 247 ms | 0.87x | `SELECT UserID FROM hits WHERE UserID = 435090932899640449` |
| 21 | 3.89 s | 3.86 s | 0.99x | `SELECT COUNT(*) FROM hits WHERE URL LIKE '%google%'` |
| 22 | 4.17 s | 2.78 s | 0.67x | `SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM hits WHERE URL LIKE '%google%' AND Se...` |
| 23 | 7.85 s | 5.34 s | 0.68x | `SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, COUNT(DISTINCT UserID) FROM h...` |
| 24 | 1.97 s | 18.29 s | 9.29x | `SELECT * FROM hits WHERE URL LIKE '%google%' ORDER BY EventTime LIMIT 10` |
| 25 | 1.71 s | 337 ms | 0.20x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10` |
| 26 | 2.84 s | 798 ms | 0.28x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY SearchPhrase LIMIT 10` |
| 27 | 4.12 s | 365 ms | 0.09x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime, SearchPhrase...` |
| 28 | 2.88 s | 3.19 s | 1.11x | `SELECT CounterID, AVG(STRLEN(URL)) AS l, COUNT(*) AS c FROM hits WHERE URL <> '' GROUP ...` |
| 29 | FAIL | 23.14 s |  | `SELECT REGEXP_REPLACE(Referer, '^https?://(?:www\.)?([^/]+)/.*$', '\1') AS k, AVG(STRLE...` |
| 30 | 139 ms | 229 ms | 1.65x | `SELECT SUM(ResolutionWidth), SUM(ResolutionWidth + 1), SUM(ResolutionWidth + 2), SUM(Re...` |
| 31 | 5.40 s | 1.62 s | 0.30x | `SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FR...` |
| 32 | 11.38 s | 1.96 s | 0.17x | `SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits...` |
| 33 | FAIL | 9.01 s |  | `SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits...` |
| 34 | 9.69 s | 10.84 s | 1.12x | `SELECT URL, COUNT(*) AS c FROM hits GROUP BY URL ORDER BY c DESC LIMIT 10` |
| 35 | 9.28 s | 10.47 s | 1.13x | `SELECT 1, URL, COUNT(*) AS c FROM hits GROUP BY 1, URL ORDER BY c DESC LIMIT 10` |
| 36 | 883 ms | 1.64 s | 1.86x | `SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, COUNT(*) AS c FROM hits GROU...` |
| 37 | 227 ms | FAIL |  | `SELECT URL, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013...` |
| 38 | 134 ms | FAIL |  | `SELECT Title, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '20...` |
| 39 | 146 ms | FAIL |  | `SELECT URL, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013...` |
| 40 | FAIL | FAIL |  | `SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN (SearchEngineID = 0 AND A...` |
| 41 | 1.45 s | FAIL |  | `SELECT URLHash, EventDate, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND Eve...` |
| 42 | 59 ms | FAIL |  | `SELECT WindowClientWidth, WindowClientHeight, COUNT(*) AS PageViews FROM hits WHERE Cou...` |
| 43 | 1.49 s | FAIL |  | `SELECT DATE_TRUNC('minute', EventTime) AS M, COUNT(*) AS PageViews FROM hits WHERE Coun...` |

**32/43 queries ran on both. SlothDB faster on 15/32. Median speedup: 0.97x.**

# Benchmark results

- Queries: `bench/clickbench/queries.sql`
- Data: `bench/clickbench/data/hits.parquet`
- Runs per query: 1 (with warmup)

| # | SlothDB | DuckDB | Speedup | Query |
|--:|--:|--:|:-:|---|
| 1 | 32 ms | 104 ms | 3.19x | `SELECT COUNT(*) FROM hits` |
| 2 | 74 ms | 169 ms | 2.27x | `SELECT COUNT(*) FROM hits WHERE AdvEngineID <> 0` |
| 3 | 181 ms | 225 ms | 1.24x | `SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM hits` |
| 4 | 179 ms | 226 ms | 1.26x | `SELECT AVG(UserID) FROM hits` |
| 5 | 962 ms | 1.03 s | 1.07x | `SELECT COUNT(DISTINCT UserID) FROM hits` |
| 6 | 1.89 s | 1.28 s | 0.68x | `SELECT COUNT(DISTINCT SearchPhrase) FROM hits` |
| 7 | 32 ms | 180 ms | 5.61x | `SELECT MIN(EventDate), MAX(EventDate) FROM hits` |
| 8 | 113 ms | 178 ms | 1.58x | `SELECT AdvEngineID, COUNT(*) FROM hits WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDE...` |
| 9 | 1.24 s | 1.28 s | 1.04x | `SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM hits GROUP BY RegionID ORDER BY u DES...` |
| 10 | 1.94 s | 1.59 s | 0.82x | `SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT ...` |
| 11 | 470 ms | 437 ms | 0.93x | `SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <...` |
| 12 | 542 ms | 495 ms | 0.91x | `SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE Mobil...` |
| 13 | 2.01 s | 1.33 s | 0.66x | `SELECT SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPh...` |
| 14 | FAIL | 2.55 s |  | `SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM hits WHERE SearchPhrase <> '' GRO...` |
| 15 | 1.90 s | 1.60 s | 0.84x | `SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' G...` |
| 16 | 1.02 s | 1.22 s | 1.20x | `SELECT UserID, COUNT(*) FROM hits GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10` |
| 17 | 4.13 s | 3.03 s | 0.73x | `SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase ORDER BY ...` |
| 18 | 4.24 s | 2.38 s | 0.56x | `SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase LIMIT 10` |
| 19 | FAIL | FAIL |  | `SELECT UserID, extract(minute FROM EventTime) AS m, SearchPhrase, COUNT(*) FROM hits GR...` |
| 20 | 275 ms | 263 ms | 0.96x | `SELECT UserID FROM hits WHERE UserID = 435090932899640449` |
| 21 | 3.78 s | 3.67 s | 0.97x | `SELECT COUNT(*) FROM hits WHERE URL LIKE '%google%'` |
| 22 | 4.26 s | 2.74 s | 0.64x | `SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM hits WHERE URL LIKE '%google%' AND Se...` |
| 23 | FAIL | 6.45 s |  | `SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, COUNT(DISTINCT UserID) FROM h...` |
| 24 | 2.04 s | 17.95 s | 8.80x | `SELECT * FROM hits WHERE URL LIKE '%google%' ORDER BY EventTime LIMIT 10` |
| 25 | 1.78 s | 340 ms | 0.19x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10` |
| 26 | 2.80 s | 782 ms | 0.28x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY SearchPhrase LIMIT 10` |
| 27 | 6.01 s | 343 ms | 0.06x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime, SearchPhrase...` |
| 28 | 2.81 s | 3.09 s | 1.10x | `SELECT CounterID, AVG(STRLEN(URL)) AS l, COUNT(*) AS c FROM hits WHERE URL <> '' GROUP ...` |
| 29 | FAIL | 22.18 s |  | `SELECT REGEXP_REPLACE(Referer, '^https?://(?:www\.)?([^/]+)/.*$', '\1') AS k, AVG(STRLE...` |
| 30 | 123 ms | 216 ms | 1.76x | `SELECT SUM(ResolutionWidth), SUM(ResolutionWidth + 1), SUM(ResolutionWidth + 2), SUM(Re...` |
| 31 | 5.38 s | 1.57 s | 0.29x | `SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FR...` |
| 32 | 11.44 s | 1.91 s | 0.17x | `SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits...` |
| 33 | FAIL | 8.19 s |  | `SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits...` |
| 34 | 9.07 s | 10.26 s | 1.13x | `SELECT URL, COUNT(*) AS c FROM hits GROUP BY URL ORDER BY c DESC LIMIT 10` |
| 35 | 9.07 s | 10.17 s | 1.12x | `SELECT 1, URL, COUNT(*) AS c FROM hits GROUP BY 1, URL ORDER BY c DESC LIMIT 10` |
| 36 | 856 ms | 1.57 s | 1.84x | `SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, COUNT(*) AS c FROM hits GROU...` |
| 37 | 214 ms | FAIL |  | `SELECT URL, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013...` |
| 38 | 125 ms | FAIL |  | `SELECT Title, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '20...` |
| 39 | 142 ms | FAIL |  | `SELECT URL, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013...` |
| 40 | FAIL | FAIL |  | `SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN (SearchEngineID = 0 AND A...` |
| 41 | 1.45 s | FAIL |  | `SELECT URLHash, EventDate, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND Eve...` |
| 42 | 55 ms | FAIL |  | `SELECT WindowClientWidth, WindowClientHeight, COUNT(*) AS PageViews FROM hits WHERE Cou...` |
| 43 | 1.43 s | FAIL |  | `SELECT DATE_TRUNC('minute', EventTime) AS M, COUNT(*) AS PageViews FROM hits WHERE Coun...` |

**31/43 queries ran on both. SlothDB faster on 15/31. Median speedup: 0.97x.**

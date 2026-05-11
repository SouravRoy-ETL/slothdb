# Benchmark results

- Queries: `bench/clickbench/queries.sql`
- Data: `bench/clickbench/data/hits.parquet`
- Runs per query: 1

| # | SlothDB | DuckDB | Speedup | Query |
|--:|--:|--:|:-:|---|
| 1 | 99 ms | 279 ms | 2.82x | `SELECT COUNT(*) FROM hits` |
| 2 | 83 ms | 183 ms | 2.21x | `SELECT COUNT(*) FROM hits WHERE AdvEngineID <> 0` |
| 3 | 227 ms | 239 ms | 1.05x | `SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM hits` |
| 4 | 197 ms | 244 ms | 1.24x | `SELECT AVG(UserID) FROM hits` |
| 5 | 1.01 s | 1.10 s | 1.09x | `SELECT COUNT(DISTINCT UserID) FROM hits` |
| 6 | 1.88 s | 1.29 s | 0.69x | `SELECT COUNT(DISTINCT SearchPhrase) FROM hits` |
| 7 | 39 ms | 192 ms | 4.93x | `SELECT MIN(EventDate), MAX(EventDate) FROM hits` |
| 8 | 120 ms | 197 ms | 1.64x | `SELECT AdvEngineID, COUNT(*) FROM hits WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDE...` |
| 9 | 1.28 s | 1.33 s | 1.03x | `SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM hits GROUP BY RegionID ORDER BY u DES...` |
| 10 | 1.98 s | 1.65 s | 0.83x | `SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT ...` |
| 11 | 492 ms | 434 ms | 0.88x | `SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <...` |
| 12 | 518 ms | 522 ms | 1.01x | `SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE Mobil...` |
| 13 | 1.99 s | 1.55 s | 0.78x | `SELECT SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPh...` |
| 14 | FAIL | 2.80 s |  | `SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM hits WHERE SearchPhrase <> '' GRO...` |
| 15 | 1.99 s | 1.67 s | 0.84x | `SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' G...` |
| 16 | 1.04 s | 1.21 s | 1.16x | `SELECT UserID, COUNT(*) FROM hits GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10` |
| 17 | 4.21 s | 3.17 s | 0.75x | `SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase ORDER BY ...` |
| 18 | 4.26 s | 2.40 s | 0.56x | `SELECT UserID, SearchPhrase, COUNT(*) FROM hits GROUP BY UserID, SearchPhrase LIMIT 10` |
| 19 | FAIL | FAIL |  | `SELECT UserID, extract(minute FROM EventTime) AS m, SearchPhrase, COUNT(*) FROM hits GR...` |
| 20 | 393 ms | 396 ms | 1.01x | `SELECT UserID FROM hits WHERE UserID = 435090932899640449` |
| 21 | 5.49 s | 3.73 s | 0.68x | `SELECT COUNT(*) FROM hits WHERE URL LIKE '%google%'` |
| 22 | 4.47 s | 2.76 s | 0.62x | `SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM hits WHERE URL LIKE '%google%' AND Se...` |
| 23 | FAIL | 6.83 s |  | `SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, COUNT(DISTINCT UserID) FROM h...` |
| 24 | 2.32 s | 20.37 s | 8.76x | `SELECT * FROM hits WHERE URL LIKE '%google%' ORDER BY EventTime LIMIT 10` |
| 25 | 1.91 s | 427 ms | 0.22x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10` |
| 26 | 3.52 s | 825 ms | 0.23x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY SearchPhrase LIMIT 10` |
| 27 | 6.19 s | 457 ms | 0.07x | `SELECT SearchPhrase FROM hits WHERE SearchPhrase <> '' ORDER BY EventTime, SearchPhrase...` |
| 28 | 4.73 s | 3.32 s | 0.70x | `SELECT CounterID, AVG(STRLEN(URL)) AS l, COUNT(*) AS c FROM hits WHERE URL <> '' GROUP ...` |
| 29 | FAIL | 24.48 s |  | `SELECT REGEXP_REPLACE(Referer, '^https?://(?:www\.)?([^/]+)/.*$', '\1') AS k, AVG(STRLE...` |
| 30 | 158 ms | 233 ms | 1.47x | `SELECT SUM(ResolutionWidth), SUM(ResolutionWidth + 1), SUM(ResolutionWidth + 2), SUM(Re...` |
| 31 | 5.73 s | 1.64 s | 0.29x | `SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FR...` |
| 32 | 16.19 s | 2.02 s | 0.12x | `SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits...` |
| 33 | FAIL | 9.29 s |  | `SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM hits...` |
| 34 | 11.26 s | 10.68 s | 0.95x | `SELECT URL, COUNT(*) AS c FROM hits GROUP BY URL ORDER BY c DESC LIMIT 10` |
| 35 | 9.34 s | 10.95 s | 1.17x | `SELECT 1, URL, COUNT(*) AS c FROM hits GROUP BY 1, URL ORDER BY c DESC LIMIT 10` |
| 36 | 858 ms | 1.66 s | 1.94x | `SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, COUNT(*) AS c FROM hits GROU...` |
| 37 | 239 ms | FAIL |  | `SELECT URL, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013...` |
| 38 | 231 ms | FAIL |  | `SELECT Title, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '20...` |
| 39 | 170 ms | FAIL |  | `SELECT URL, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013...` |
| 40 | FAIL | FAIL |  | `SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN (SearchEngineID = 0 AND A...` |
| 41 | 1.51 s | FAIL |  | `SELECT URLHash, EventDate, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND Eve...` |
| 42 | 72 ms | FAIL |  | `SELECT WindowClientWidth, WindowClientHeight, COUNT(*) AS PageViews FROM hits WHERE Cou...` |
| 43 | 1.57 s | FAIL |  | `SELECT DATE_TRUNC('minute', EventTime) AS M, COUNT(*) AS PageViews FROM hits WHERE Coun...` |

**31/43 queries ran on both. SlothDB faster on 15/31. Median speedup: 0.95x.**

#!/bin/bash
# Quick unblock checker — runs each query once, captures count + timing.
SLOTH="build/src/Release/slothdb.exe"
DUCK="real-life-testing/duckdb.exe"
DATA="bench/clickbench/data/hits.parquet"

run_one() {
  local n="$1"; local q="$2"
  echo "--- Q$n ---"
  local t0=$(date +%s%N)
  local s_out=$("$SLOTH" -c "$q" 2>&1 | tail -3 | head -1)
  local t1=$(date +%s%N)
  local s_ms=$(( (t1-t0)/1000000 ))
  echo "S[$s_ms ms]: $s_out"
}

run_one 22 "SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '$DATA' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 1"
run_one 23 "SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, COUNT(DISTINCT UserID) FROM '$DATA' WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 1"
run_one 28 "SELECT CounterID, AVG(STRLEN(URL)) AS l, COUNT(*) AS c FROM '$DATA' WHERE URL <> '' GROUP BY CounterID HAVING COUNT(*) > 100000 ORDER BY l DESC LIMIT 1"
run_one 31 "SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '$DATA' WHERE SearchPhrase <> '' GROUP BY SearchEngineID, ClientIP ORDER BY c DESC LIMIT 1"
run_one 32 "SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '$DATA' WHERE SearchPhrase <> '' GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 1"
run_one 33 "SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '$DATA' GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 1"
run_one 34 "SELECT URL, COUNT(*) AS c FROM '$DATA' GROUP BY URL ORDER BY c DESC LIMIT 1"
run_one 35 "SELECT 1, URL, COUNT(*) AS c FROM '$DATA' GROUP BY 1, URL ORDER BY c DESC LIMIT 1"
run_one 36 "SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, COUNT(*) AS c FROM '$DATA' GROUP BY ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3 ORDER BY c DESC LIMIT 1"
run_one 40 "SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN Referer ELSE '' END AS Src, URL AS Dst, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst ORDER BY PageViews DESC LIMIT 1 OFFSET 1000"
echo "--- DONE ---"

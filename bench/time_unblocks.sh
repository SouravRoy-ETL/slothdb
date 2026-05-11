#!/bin/bash
# Time queries we expect to work, measure SlothDB vs DuckDB ratio.
SLOTH="build/src/Release/slothdb.exe"
DUCK="real-life-testing/duckdb.exe"
DATA="bench/clickbench/data/hits.parquet"

time_one() {
  local n="$1"; local q="$2"; local q_duck="$3"
  # Use 2nd run as warm
  "$SLOTH" -c "$q" >/dev/null 2>&1
  local t0=$(date +%s%N)
  "$SLOTH" -c "$q" >/dev/null 2>&1
  local t1=$(date +%s%N)
  local s_ms=$(( (t1-t0)/1000000 ))

  "$DUCK" -c "$q_duck" >/dev/null 2>&1
  local d0=$(date +%s%N)
  "$DUCK" -c "$q_duck" >/dev/null 2>&1
  local d1=$(date +%s%N)
  local d_ms=$(( (d1-d0)/1000000 ))

  local ratio=$(echo "scale=2; $d_ms/$s_ms" | bc)
  echo "Q$n: SlothDB=${s_ms}ms DuckDB=${d_ms}ms ratio=${ratio}x"
}

# Confirmed byte-equal unblocks; date filter 2013-07 = days 15887-15917 for DuckDB
time_one 22 \
  "SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '$DATA' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10" \
  "SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '$DATA' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"

time_one 37 \
  "SELECT URL, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND DontCountHits = 0 AND IsRefresh = 0 AND URL <> '' GROUP BY URL ORDER BY PageViews DESC LIMIT 10" \
  "SELECT URL, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= 15887 AND EventDate <= 15917 AND DontCountHits = 0 AND IsRefresh = 0 AND URL <> '' GROUP BY URL ORDER BY PageViews DESC LIMIT 10"

time_one 38 \
  "SELECT Title, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND DontCountHits = 0 AND IsRefresh = 0 AND Title <> '' GROUP BY Title ORDER BY PageViews DESC LIMIT 10" \
  "SELECT Title, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= 15887 AND EventDate <= 15917 AND DontCountHits = 0 AND IsRefresh = 0 AND Title <> '' GROUP BY Title ORDER BY PageViews DESC LIMIT 10"

time_one 39 \
  "SELECT URL, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 AND DontCountHits = 0 GROUP BY URL ORDER BY PageViews DESC LIMIT 10" \
  "SELECT URL, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= 15887 AND EventDate <= 15917 AND IsRefresh = 0 AND DontCountHits = 0 GROUP BY URL ORDER BY PageViews DESC LIMIT 10"

time_one 41 \
  "SELECT URLHash, EventDate, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 AND TraficSourceID IN (-1, 6) AND RefererHash = 3594120000172545465 GROUP BY URLHash, EventDate ORDER BY PageViews DESC LIMIT 10 OFFSET 100" \
  "SELECT URLHash, EventDate, COUNT(*) AS PageViews FROM '$DATA' WHERE CounterID = 62 AND EventDate >= 15887 AND EventDate <= 15917 AND IsRefresh = 0 AND TraficSourceID IN (-1, 6) AND RefererHash = 3594120000172545465 GROUP BY URLHash, EventDate ORDER BY PageViews DESC LIMIT 10 OFFSET 100"

echo "DONE"

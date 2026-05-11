"""Q40 CASE bottleneck diagnostic."""
import subprocess, time
from pathlib import Path
SLOTH = str((Path(__file__).resolve().parents[1] / 'build/src/Release/slothdb.exe').resolve())

queries = {
    "no-CASE 5-col": "SELECT TraficSourceID, SearchEngineID, AdvEngineID, Referer AS Src, URL AS Dst, COUNT(*) AS PageViews FROM 'bench/clickbench/data/hits.parquet' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst ORDER BY PageViews DESC LIMIT 10 OFFSET 1000",
    "Q40 with CASE": "SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN Referer ELSE '' END AS Src, URL AS Dst, COUNT(*) AS PageViews FROM 'bench/clickbench/data/hits.parquet' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst ORDER BY PageViews DESC LIMIT 10 OFFSET 1000",
    "CASE-only no-group": "SELECT COUNT(*) FROM 'bench/clickbench/data/hits.parquet' WHERE CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN 1 ELSE 0 END = 1",
    "Q40 filtered subq + CASE": "SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN Referer ELSE '' END AS Src, URL AS Dst, COUNT(*) AS PageViews FROM (SELECT TraficSourceID, SearchEngineID, AdvEngineID, Referer, URL FROM 'bench/clickbench/data/hits.parquet' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0) t GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst ORDER BY PageViews DESC LIMIT 10 OFFSET 1000",
    "CASE-eval no-grp filtered": "SELECT COUNT(*) FROM 'bench/clickbench/data/hits.parquet' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 AND (CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN 1 ELSE 0 END = 1)",
}
for label, sql in queries.items():
    t0 = time.perf_counter()
    try:
        p = subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=30)
        dt = (time.perf_counter() - t0)*1000
        if p.returncode == 0:
            print(f'{label:<24s}: {dt:>7.0f}ms')
        else:
            err = p.stderr.decode("utf-8", "replace")[:120]
            print(f'{label:<24s}: ERROR {err}')
    except subprocess.TimeoutExpired:
        print(f'{label:<24s}: TIMEOUT >30s')

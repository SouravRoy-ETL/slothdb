import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

# Queries that likely hit FUSED GENERIC packed path (multi-col GROUP BY or single-col + DISTINCT)
queries = {
    9:  f"SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM '{DATA}' GROUP BY RegionID ORDER BY u DESC LIMIT 10",
    10: f"SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT UserID) FROM '{DATA}' GROUP BY RegionID ORDER BY c DESC LIMIT 10",
    31: f"SELECT URLHash, EventDate, COUNT(*) AS PageViews FROM '{DATA}' WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 AND TraficSourceID IN (-1, 6) AND RefererHash = 3594120000172545465 GROUP BY URLHash, EventDate ORDER BY PageViews DESC LIMIT 10 OFFSET 100",
    32: f"SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 10",
    33: f"SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 10",
}

print('='*70, flush=True)
for q, sql in queries.items():
    s_times = []
    for _ in range(3):
        t = time.perf_counter()
        try:
            subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            s_times = [60.0]
            break
        s_times.append(time.perf_counter() - t)
    sloth = min(s_times) * 1000
    d_times = []
    for _ in range(3):
        t = time.perf_counter()
        try:
            subprocess.run([DUCK, '-c', sql], capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            d_times = [60.0]
            break
        d_times.append(time.perf_counter() - t)
    duck = min(d_times) * 1000
    ratio = duck / sloth
    status = 'WIN' if ratio > 1.0 else 'LOSS'
    print(f'Q{q:>2} {status} sloth={sloth:>6.0f}ms duck={duck:>6.0f}ms ratio={ratio:.2f}x', flush=True)

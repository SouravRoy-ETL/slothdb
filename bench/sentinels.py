"""Wide sentinel run with 5 trials per query."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

queries = {
    1: f"SELECT COUNT(*) FROM '{DATA}'",
    3: f"SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM '{DATA}'",
    4: f"SELECT AVG(UserID) FROM '{DATA}'",
    5: f"SELECT COUNT(DISTINCT UserID) FROM '{DATA}'",
    7: f"SELECT MIN(EventDate), MAX(EventDate) FROM '{DATA}'",
    8: f"SELECT AdvEngineID, COUNT(*) FROM '{DATA}' WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC",
    9: f"SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM '{DATA}' GROUP BY RegionID ORDER BY u DESC LIMIT 10",
    10: f"SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT UserID) FROM '{DATA}' GROUP BY RegionID ORDER BY c DESC LIMIT 10",
    11: f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{DATA}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    12: f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{DATA}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10",
    20: f"SELECT UserID FROM '{DATA}' WHERE UserID = 435090932899640449",
    21: f"SELECT COUNT(*) FROM '{DATA}' WHERE URL LIKE '%google%'",
    22: f"SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '{DATA}' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
    24: f"SELECT * FROM '{DATA}' WHERE URL LIKE '%google%' ORDER BY EventTime LIMIT 10",
    28: f"SELECT CounterID, AVG(STRLEN(URL)) AS l, COUNT(*) AS c FROM '{DATA}' WHERE URL <> '' GROUP BY CounterID HAVING COUNT(*) > 100000 ORDER BY l DESC LIMIT 25",
    30: f"SELECT SUM(ResolutionWidth), SUM(ResolutionWidth + 1), SUM(ResolutionWidth + 2), SUM(ResolutionWidth + 3) FROM '{DATA}'",
}

print('='*70, flush=True)
N = 5
for q, sql in queries.items():
    times = []
    try:
        for _ in range(N):
            t = time.perf_counter()
            subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=30)
            times.append(time.perf_counter() - t)
        sloth = min(times) * 1000
    except subprocess.TimeoutExpired:
        print(f'Q{q:>2} sloth=TIMEOUT', flush=True); continue
    times = []
    try:
        for _ in range(N):
            t = time.perf_counter()
            subprocess.run([DUCK, '-c', sql], capture_output=True, timeout=30)
            times.append(time.perf_counter() - t)
        duck = min(times) * 1000
    except subprocess.TimeoutExpired:
        duck = -1
    if duck > 0:
        ratio = duck / sloth
        status = 'WIN' if ratio > 1.0 else 'LOSS'
        print(f'Q{q:>2} {status} sloth={sloth:.0f}ms duck={duck:.0f}ms ratio={ratio:.2f}x', flush=True)
    else:
        print(f'Q{q:>2} sloth={sloth:.0f}ms duck=TIMEOUT', flush=True)

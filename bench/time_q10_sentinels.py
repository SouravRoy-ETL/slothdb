import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

queries = {
    1: f"SELECT COUNT(*) FROM '{DATA}'",
    5: f"SELECT COUNT(DISTINCT UserID) FROM '{DATA}'",
    7: f"SELECT MIN(EventDate), MAX(EventDate) FROM '{DATA}'",
    8: f"SELECT AdvEngineID, COUNT(*) FROM '{DATA}' WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC",
    10: f"SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT UserID) FROM '{DATA}' GROUP BY RegionID ORDER BY c DESC LIMIT 10",
    11: f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{DATA}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    12: f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{DATA}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10",
    30: f"SELECT SUM(ResolutionWidth), SUM(ResolutionWidth + 1), SUM(ResolutionWidth + 2), SUM(ResolutionWidth + 3) FROM '{DATA}'",
}

print('='*70, flush=True)
for q, sql in queries.items():
    s_times = []
    for _ in range(5):
        t = time.perf_counter()
        subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=60)
        s_times.append(time.perf_counter() - t)
    sloth = min(s_times) * 1000
    d_times = []
    for _ in range(5):
        t = time.perf_counter()
        subprocess.run([DUCK, '-c', sql], capture_output=True, timeout=60)
        d_times.append(time.perf_counter() - t)
    duck = min(d_times) * 1000
    ratio = duck / sloth
    status = 'WIN' if ratio > 1.0 else 'LOSS'
    print(f'Q{q:>2} {status} sloth={sloth:>5.0f}ms duck={duck:>5.0f}ms ratio={ratio:.2f}x', flush=True)

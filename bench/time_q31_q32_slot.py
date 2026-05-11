"""Time Q31 (INT+INT GROUP BY) + Q32 (BIGINT+INT GROUP BY) for slot-map A/B."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')
queries = {
    'Q31_INT_INT': f"SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchEngineID, ClientIP ORDER BY c DESC LIMIT 10",
    'Q32_BIGINT_INT': f"SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 10",
}
N = 3
for name, sql in queries.items():
    times = []
    try:
        for _ in range(N):
            t = time.perf_counter()
            subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=60)
            times.append(time.perf_counter() - t)
        sloth = min(times) * 1000
    except subprocess.TimeoutExpired:
        print(f'{name} sloth=TIMEOUT', flush=True); continue
    times = []
    try:
        for _ in range(N):
            t = time.perf_counter()
            subprocess.run([DUCK, '-c', sql], capture_output=True, timeout=60)
            times.append(time.perf_counter() - t)
        duck = min(times) * 1000
    except subprocess.TimeoutExpired:
        duck = -1
    if duck > 0:
        ratio = duck / sloth
        status = 'WIN' if ratio > 1.0 else 'LOSS'
        print(f'{name} sloth={sloth:7.0f}ms duck={duck:7.0f}ms ratio={ratio:.2f}x {status}', flush=True)
    else:
        print(f'{name} sloth={sloth:7.0f}ms duck=TIMEOUT', flush=True)

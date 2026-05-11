"""Time the TIMEOUT/big-LOSS queries."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

queries = {
    13: f"SELECT SearchPhrase, COUNT(*) AS c FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
    14: f"SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY u DESC LIMIT 10",
    15: f"SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchEngineID, SearchPhrase ORDER BY c DESC LIMIT 10",
    16: f"SELECT UserID, COUNT(*) FROM '{DATA}' GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10",
    17: f"SELECT UserID, SearchPhrase, COUNT(*) FROM '{DATA}' GROUP BY UserID, SearchPhrase ORDER BY COUNT(*) DESC LIMIT 10",
    18: f"SELECT UserID, SearchPhrase, COUNT(*) FROM '{DATA}' GROUP BY UserID, SearchPhrase LIMIT 10",
    25: f"SELECT SearchPhrase FROM '{DATA}' WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10",
    26: f"SELECT SearchPhrase FROM '{DATA}' WHERE SearchPhrase <> '' ORDER BY SearchPhrase LIMIT 10",
    27: f"SELECT SearchPhrase FROM '{DATA}' WHERE SearchPhrase <> '' ORDER BY EventTime, SearchPhrase LIMIT 10",
    31: f"SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchEngineID, ClientIP ORDER BY c DESC LIMIT 10",
    32: f"SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 10",
    33: f"SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 10",
    34: f"SELECT URL, COUNT(*) AS c FROM '{DATA}' GROUP BY URL ORDER BY c DESC LIMIT 10",
    35: f"SELECT 1, URL, COUNT(*) AS c FROM '{DATA}' GROUP BY 1, URL ORDER BY c DESC LIMIT 10",
    36: f"SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, COUNT(*) AS c FROM '{DATA}' GROUP BY ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3 ORDER BY c DESC LIMIT 10",
}

for q, sql in queries.items():
    times = []
    try:
        for _ in range(2):
            t = time.perf_counter()
            r = subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=60)
            times.append(time.perf_counter() - t)
        sloth = min(times) * 1000
    except subprocess.TimeoutExpired:
        sloth = -1
    times = []
    try:
        for _ in range(2):
            t = time.perf_counter()
            subprocess.run([DUCK, '-c', sql], capture_output=True, timeout=60)
            times.append(time.perf_counter() - t)
        duck = min(times) * 1000
    except subprocess.TimeoutExpired:
        duck = -1
    sloth_str = f'{sloth:.0f}ms' if sloth > 0 else 'TIMEOUT'
    duck_str = f'{duck:.0f}ms' if duck > 0 else 'TIMEOUT'
    if sloth > 0 and duck > 0:
        ratio = duck / sloth
        status = 'WIN' if ratio > 1.0 else 'LOSS'
        print(f'Q{q:>2} {status} sloth={sloth_str} duck={duck_str} ratio={ratio:.2f}x', flush=True)
    else:
        print(f'Q{q:>2} sloth={sloth_str} duck={duck_str}', flush=True)

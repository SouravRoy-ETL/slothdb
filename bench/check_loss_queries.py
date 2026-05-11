import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

# All non-WIN queries from survey (focus on testable ones)
queries = {
    6:  f"SELECT COUNT(DISTINCT SearchPhrase) FROM '{DATA}'",
    14: f"SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY u DESC LIMIT 10",
    16: f"SELECT UserID, COUNT(*) FROM '{DATA}' GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10",
    22: f"SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '{DATA}' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
    25: f"SELECT UserID, COUNT(*) FROM '{DATA}' GROUP BY UserID ORDER BY COUNT(*) DESC LIMIT 10",
    26: f"SELECT UserID, SearchPhrase, COUNT(*) FROM '{DATA}' GROUP BY UserID, SearchPhrase ORDER BY COUNT(*) DESC LIMIT 10",
    27: f"SELECT UserID, SearchPhrase, COUNT(*) FROM '{DATA}' GROUP BY UserID, SearchPhrase LIMIT 10",
    36: f"SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, COUNT(*) AS c FROM '{DATA}' GROUP BY ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3 ORDER BY c DESC LIMIT 10",
}

print('='*70, flush=True)
for q, sql in queries.items():
    s_times = []
    for _ in range(2):  # 2 trials only for slow queries
        t = time.perf_counter()
        try:
            subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            s_times = [30.0]
            break
        s_times.append(time.perf_counter() - t)
    sloth = min(s_times) * 1000
    d_times = []
    for _ in range(2):
        t = time.perf_counter()
        try:
            subprocess.run([DUCK, '-c', sql], capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            d_times = [30.0]
            break
        d_times.append(time.perf_counter() - t)
    duck = min(d_times) * 1000
    ratio = duck / sloth if sloth > 0 else 0
    status = 'WIN' if ratio > 1.0 else 'LOSS'
    flag = ' TIMEOUT' if sloth >= 29900 else ''
    print(f'Q{q:>2} {status}{flag} sloth={sloth:>6.0f}ms duck={duck:>6.0f}ms ratio={ratio:.2f}x', flush=True)

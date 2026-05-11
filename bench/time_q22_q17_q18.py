"""Bench Q22 + Q17 + Q18 (2-col Q15-shape candidates) after skip-str_gc patch."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
P = os.path.abspath('bench/clickbench/data/hits.parquet').replace(os.sep, '/')

queries = {
    'Q15':     f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    'Q17':     f"SELECT SearchPhrase, COUNT(*) AS c FROM '{P}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
    'Q18':     f"SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM '{P}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY u DESC LIMIT 10",
    'Q22_2col_LIMIT': f"SELECT UserID, SearchPhrase, COUNT(*) FROM '{P}' GROUP BY UserID, SearchPhrase LIMIT 10",
    'Q31_2col': f"SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{P}' WHERE SearchPhrase <> '' GROUP BY SearchEngineID, ClientIP ORDER BY c DESC LIMIT 10",
}

def time_n(exe, sql, n=2):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try: subprocess.run([exe, '-c', sql], capture_output=True, timeout=180)
        except Exception: return None
        times.append(time.perf_counter() - t)
    return times

print(f"{'Q':18} {'sloth_ms':>10} {'duck_ms':>10} {'ratio':>8}")
for name, sql in queries.items():
    ts = time_n(S, sql); td = time_n(D, sql)
    if not ts or not td: print(f'{name}: skip'); continue
    s = min(ts) * 1000; d = min(td) * 1000
    print(f"{name:18} {s:10.0f} {d:10.0f} {d/s:>8.2f}x")

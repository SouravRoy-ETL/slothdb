"""Sentinel + Q13-base bench after str_data skip patch."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
P = os.path.abspath('bench/clickbench/data/hits.parquet').replace(os.sep, '/')

queries = {
    'Q1':  f"SELECT COUNT(*) FROM '{P}'",
    'Q2':  f"SELECT COUNT(*) FROM '{P}' WHERE AdvEngineID <> 0",
    'Q7':  f"SELECT AdvEngineID, COUNT(*) FROM '{P}' WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC",
    'Q8':  f"SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM '{P}' GROUP BY RegionID ORDER BY u DESC LIMIT 10",
    'Q30': f"SELECT SUM(ResolutionWidth), SUM(ResolutionWidth + 1), SUM(ResolutionWidth + 2), SUM(ResolutionWidth + 3) FROM '{P}'",
    'Q11_base': f"SELECT MobilePhoneModel, COUNT(*) AS c FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY c DESC LIMIT 10",
}

def time_n(exe, sql, n=4):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try: subprocess.run([exe, '-c', sql], capture_output=True, timeout=120)
        except Exception: return None
        times.append(time.perf_counter() - t)
    return times

print(f"{'Q':10} {'sloth_ms':>10} {'duck_ms':>10} {'ratio':>8}")
for name, sql in queries.items():
    ts = time_n(S, sql); td = time_n(D, sql)
    if not ts or not td: continue
    s = min(ts) * 1000; d = min(td) * 1000
    print(f"{name:10} {s:10.0f} {d:10.0f} {d/s:>8.2f}x")

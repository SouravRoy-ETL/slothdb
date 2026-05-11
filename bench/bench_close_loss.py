"""High-trial timing for close-to-flip queries: Q10, Q11, Q12, Q21."""
import subprocess, time, os, statistics
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
P = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

queries = [
    ('Q10', f"SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT UserID) FROM '{P}' GROUP BY RegionID ORDER BY c DESC LIMIT 10"),
    ('Q11', f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10"),
    ('Q12', f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10"),
    ('Q21', f"SELECT COUNT(*) FROM '{P}' WHERE URL LIKE '%google%'"),
    ('Q22', f"SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '{P}' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"),
]

def time_n(exe, sql, n):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try:
            subprocess.run([exe, '-c', sql], capture_output=True, timeout=120)
        except Exception:
            return None
        times.append(time.perf_counter() - t)
    return times

N = 8
for name, sql in queries:
    ts = time_n(S, sql, N)
    td = time_n(D, sql, N)
    if not ts or not td:
        print(f'{name}: skipped'); continue
    s_min, s_med = min(ts), statistics.median(ts)
    d_min, d_med = min(td), statistics.median(td)
    print(f'{name} sloth_min={s_min*1000:6.0f} sloth_med={s_med*1000:6.0f} duck_min={d_min*1000:6.0f} duck_med={d_med*1000:6.0f} ratio_min={d_min/s_min:.3f} ratio_med={d_med/s_med:.3f}')

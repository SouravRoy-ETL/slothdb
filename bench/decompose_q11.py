"""Decompose Q11 to find where the 320ms gap goes."""
import subprocess, time, os, statistics
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
P = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

queries = [
    ('A_count_all',  f"SELECT COUNT(*) FROM '{P}'"),
    ('B_count_filt', f"SELECT COUNT(*) FROM '{P}' WHERE MobilePhoneModel <> ''"),
    ('C_dist_uid',   f"SELECT COUNT(DISTINCT UserID) FROM '{P}'"),
    ('D_groupcount', f"SELECT MobilePhoneModel, COUNT(*) AS c FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY c DESC LIMIT 10"),
    ('E_q11_real',   f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10"),
]

def time_n(exe, sql, n=5):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try:
            subprocess.run([exe, '-c', sql], capture_output=True, timeout=120)
        except Exception:
            return None
        times.append(time.perf_counter() - t)
    return times

for name, sql in queries:
    ts = time_n(S, sql, 5)
    td = time_n(D, sql, 5)
    if not ts or not td:
        print(f'{name}: skipped'); continue
    s_min = min(ts)
    d_min = min(td)
    gap = (s_min - d_min) * 1000
    print(f'{name:14s} sloth_min={s_min*1000:5.0f}ms duck_min={d_min*1000:5.0f}ms gap={gap:+5.0f}ms ratio={d_min/s_min:.3f}x')

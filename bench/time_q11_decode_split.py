"""Time individual columns to find Q11_real decode bottleneck."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
P = os.path.abspath('bench/clickbench/data/hits.parquet').replace(os.sep, '/')

queries = {
    'A_count':       f"SELECT COUNT(*) FROM '{P}'",
    'B_count_mpm':   f"SELECT COUNT(*) FROM '{P}' WHERE MobilePhoneModel <> ''",
    'C_count_uid':   f"SELECT COUNT(UserID) FROM '{P}' WHERE MobilePhoneModel <> ''",  # decode UID + MPM
    'D_min_uid':     f"SELECT MIN(UserID), MAX(UserID) FROM '{P}' WHERE MobilePhoneModel <> ''",
    'E_q11_base':    f"SELECT MobilePhoneModel, COUNT(*) AS c FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY c DESC LIMIT 10",
    'F_q11_with_uid_min': f"SELECT MobilePhoneModel, COUNT(*), MIN(UserID), MAX(UserID) FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY 2 DESC LIMIT 10",
    'G_q11_real':    f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
}

def time_n(exe, sql, env=None, n=3):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try: subprocess.run([exe, '-c', sql], capture_output=True, timeout=120, env=env)
        except Exception: return None
        times.append(time.perf_counter() - t)
    return times

print(f"{'Q':22} {'sloth_ms':>10} {'duck_ms':>10} {'ratio':>8}")
for name, sql in queries.items():
    ts = time_n(S, sql); td = time_n(D, sql)
    if not ts or not td: continue
    s = min(ts) * 1000; d = min(td) * 1000
    print(f"{name:22} {s:10.0f} {d:10.0f} {d/s:>8.2f}x")

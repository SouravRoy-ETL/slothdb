"""Bench queries affected by str_data skip patch + safety sentinels."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
P = os.path.abspath('bench/clickbench/data/hits.parquet').replace(os.sep, '/')

# All queries that hit FUSED PARQUET 1col group path, plus sentinels.
queries = {
    # FUSED PARQUET 1col group + COUNT(*) only (Q13_eligible_skip = TRUE)
    'Q11_base':     f"SELECT MobilePhoneModel, COUNT(*) AS c FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY c DESC LIMIT 10",
    'Q13':          f"SELECT SearchPhrase, COUNT(*) AS c FROM '{P}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
    # FUSED PARQUET 1col group + non-CountStar (Q13_eligible_skip = FALSE → unchanged)
    'Q11_real':     f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    'Q15':          f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",  # actual Q15
    # Multi-col group (different path, should be unchanged)
    'Q34_URL':      f"SELECT URL, COUNT(*) AS c FROM '{P}' GROUP BY URL ORDER BY c DESC LIMIT 10",
    # Sentinels
    'Q1':           f"SELECT COUNT(*) FROM '{P}'",
    'Q7_AdvEng':    f"SELECT AdvEngineID, COUNT(*) FROM '{P}' WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC",
    'Q22_URL':      f"SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '{P}' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
}

def time_n(exe, sql, n=3):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try: subprocess.run([exe, '-c', sql], capture_output=True, timeout=120)
        except Exception: return None
        times.append(time.perf_counter() - t)
    return times

print(f"{'Q':12} {'sloth_ms':>10} {'duck_ms':>10} {'ratio':>8}")
for name, sql in queries.items():
    ts = time_n(S, sql); td = time_n(D, sql)
    if not ts or not td: print(f'{name}: skip'); continue
    s = min(ts) * 1000; d = min(td) * 1000
    print(f"{name:12} {s:10.0f} {d:10.0f} {d/s:>8.2f}x")

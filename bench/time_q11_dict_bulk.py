"""Time Q11 + Q34 high-trial-count post dict-bulk patch."""
import subprocess, time, os, statistics
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

queries = [
    ('Q11', f"SELECT SearchPhrase, COUNT(*) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY 2 DESC LIMIT 10"),
    ('Q34', f"SELECT URL, COUNT(*) FROM '{DATA}' GROUP BY URL ORDER BY 2 DESC LIMIT 10"),
]

def time_n(exe, sql, n):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try:
            subprocess.run([exe, '-c', sql], capture_output=True, timeout=120)
        except Exception as e:
            print(f'  err: {e}')
            return None
        times.append(time.perf_counter() - t)
    return times

N = 10
for name, sql in queries:
    ts = time_n(S, sql, N)
    td = time_n(D, sql, N)
    if not ts or not td:
        print(f'{name}: skipped'); continue
    s_min, s_med = min(ts), statistics.median(ts)
    d_min, d_med = min(td), statistics.median(td)
    print(f'{name} sloth_min={s_min*1000:.0f}ms sloth_med={s_med*1000:.0f}ms duck_min={d_min*1000:.0f}ms duck_med={d_med*1000:.0f}ms ratio_min={d_min/s_min:.3f}x ratio_med={d_med/s_med:.3f}x')

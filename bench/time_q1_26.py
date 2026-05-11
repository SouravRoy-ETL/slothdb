"""Bench Q1-Q26 to find remaining loss queries."""
import subprocess, time, os, re
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
P = os.path.abspath('bench/clickbench/data/hits.parquet').replace(os.sep, '/')

queries = []
with open('bench/clickbench/queries.sql') as f:
    for i, line in enumerate(f, 1):
        line = line.rstrip()
        if not line or line.lstrip().startswith('--'): continue
        if line.endswith(';'): line = line[:-1]
        queries.append((i, re.sub(r"\bhits\b", f"'{P}'", line)))

def time_n(exe, sql, n=1):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try:
            r = subprocess.run([exe, '-c', sql], capture_output=True, timeout=120)
            ok = r.returncode == 0 and not (b"Error" in r.stdout or b"Error" in r.stderr)
        except Exception: return None, "fail"
        if not ok: return None, "fail"
        times.append(time.perf_counter() - t)
    return times, "ok"

print(f"{'Q':4} {'sloth_ms':>10} {'duck_ms':>10} {'ratio':>8}  preview")
for q, sql in queries[:26]:
    ts, ts_status = time_n(S, sql); td, td_status = time_n(D, sql)
    s = min(ts) * 1000 if ts else 999999
    d = min(td) * 1000 if td else 999999
    ratio = (d / s) if s and d else 0
    preview = sql[:55].replace("'" + P + "'", "hits")
    print(f"Q{q:<3} {s:10.0f} {d:10.0f} {ratio:>8.2f}x  {preview}")

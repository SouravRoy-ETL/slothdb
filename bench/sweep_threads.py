"""Sweep SLOTH_MAXTHREADS for given query ids. DuckDB run once as baseline.
Usage: python bench/sweep_threads.py <qid> [<qid> ...]
"""
import os, subprocess, sys, time, re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
QFILE = ROOT / "bench/clickbench/queries.sql"
TIMEOUT = 30

def queries():
    qs = []
    for l in QFILE.read_text(encoding="utf-8").splitlines():
        s = l.strip()
        if s and not s.startswith("--"):
            qs.append(s[:-1] if s.endswith(";") else s)
    return qs

def run(exe, sql, env):
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT, env=env)
        return time.perf_counter() - t0, p.returncode
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), -1

def best(exe, sql, env, n=5):
    b = float("inf")
    for _ in range(n):
        dt, rc = run(exe, sql, env)
        if rc != 0:
            return dt, rc
        b = min(b, dt)
    return b, 0

def main():
    qs = queries()
    base = os.environ.copy(); base["PYTHONIOENCODING"] = "utf-8"
    for qid in [int(x) for x in sys.argv[1:]]:
        sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", qs[qid - 1])
        dd, dr = best(DUCK, sql, base)
        print(f"Q{qid}: DUCK={dd*1000:.0f}ms (rc={dr})")
        for nt in [6, 8, 10, 12]:
            env = base.copy(); env["SLOTH_MAXTHREADS"] = str(nt)
            sd, rc = best(SLOTH, sql, env)
            ratio = dd / sd if sd else 0
            print(f"  nt={nt:2d}: SLOTH={sd*1000:5.0f}ms  ratio={ratio:.3f}x  "
                  f"{'WIN' if ratio >= 1.0 else 'LOSS'}{'  (rc=%d)'%rc if rc else ''}")

if __name__ == "__main__":
    main()

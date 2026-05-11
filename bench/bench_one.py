"""Quick wall-time micro-driver: SLOTH and DUCK, 30s timeout, min of 5.
Usage: python bench/bench_one.py <q_id>
"""
import os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
QFILE = ROOT / "bench/clickbench/queries.sql"

def queries():
    qs = []
    for l in QFILE.read_text(encoding="utf-8").splitlines():
        s = l.strip()
        if s and not s.startswith("--"):
            qs.append(s[:-1] if s.endswith(";") else s)
    return qs

def run(exe, sql, timeout=30):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=timeout, env=env)
        dt = time.perf_counter() - t0
        return dt, p.returncode
    except subprocess.TimeoutExpired:
        return float(timeout), -1

def min_of(exe, sql, n, timeout=30):
    best = float("inf")
    for _ in range(n):
        dt, rc = run(exe, sql, timeout)
        if rc != 0: return dt, rc
        if dt < best: best = dt
    return best, 0

def main():
    qs = queries()
    qid = int(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    sql = qs[qid - 1].replace("hits", f"'{DATA}'") if "hits" in qs[qid - 1] else qs[qid - 1]
    # Robust hits -> data replace (word boundary)
    import re
    sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", qs[qid - 1])
    print(f"Q{qid}: {sql[:140]}{'...' if len(sql) > 140 else ''}")
    sd, sr = min_of(SLOTH, sql, n)
    dd, dr = min_of(DUCK, sql, n)
    ratio = (dd/sd) if (sd and dd) else 0
    print(f"  SLOTH = {sd*1000:.0f}ms (rc={sr})  DUCK = {dd*1000:.0f}ms (rc={dr})  ratio = {ratio:.2f}x  {'WIN' if ratio>=1.0 else 'LOSS'}")

if __name__ == "__main__":
    main()

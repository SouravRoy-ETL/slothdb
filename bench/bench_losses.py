"""Bench just the LOSS/TIMEOUT queries: min-of-3, 30s timeout per trial."""
import os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
QFILE = ROOT / "bench/clickbench/queries.sql"
TIMEOUT = 30

# 1-based ClickBench Q-numbers that are currently LOSS or TIMEOUT
TARGETS = [6, 10, 11, 12, 13, 14, 15, 17, 19, 22, 23, 26, 27, 29, 31, 32, 33]

def queries():
    qs = []
    for l in QFILE.read_text(encoding="utf-8").splitlines():
        s = l.strip()
        if s and not s.startswith("--"):
            qs.append(s[:-1] if s.endswith(";") else s)
    return qs

def run(exe, sql):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT, env=env)
        dt = time.perf_counter() - t0
        return dt, p.returncode
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), -1

def min_of(exe, sql, n):
    best = float("inf"); rc = 0
    for _ in range(n):
        dt, r = run(exe, sql)
        if r != 0:
            return dt, r
        if dt < best:
            best = dt
    return best, rc

def main():
    qs = queries()
    import re
    print(f"{'Q':>3} {'Sloth':>7} {'Duck':>7} {'Ratio':>6}  Verdict")
    print("-" * 50)
    for q in TARGETS:
        sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", qs[q - 1])
        sd, sr = min_of(SLOTH, sql, 3)
        dd, dr = min_of(DUCK, sql, 3)
        ratio = (dd/sd) if (sd and dd) else 0
        verdict = "WIN" if ratio >= 1.0 else ("TIMEOUT" if sr != 0 else "LOSS")
        print(f"Q{q:<2} {sd*1000:>5.0f}ms {dd*1000:>5.0f}ms {ratio:>5.2f}x  {verdict}")

if __name__ == "__main__":
    main()

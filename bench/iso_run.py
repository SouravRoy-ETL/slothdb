"""Isolated 3-trial query timer: SlothDB vs DuckDB. 30s hard cap.
Usage: python bench/iso_run.py <qnum> [<qnum> ...]
Prints both engines' best-of-3 times for each Q in queries.sql (line = 4 + qnum).
"""
import os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = (ROOT / "bench/clickbench/data/hits.parquet").as_posix()
QFILE = ROOT / "bench/clickbench/queries.sql"
TIMEOUT = 30

qs = [s.rstrip(";") for s in (l.strip() for l in QFILE.read_text(encoding="utf-8").splitlines())
      if s and not s.startswith("--")]

def run_once(exe, sql):
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT)
        dt = time.perf_counter() - t0
        return dt, p.returncode, p.stdout.decode("utf-8","replace"), p.stderr.decode("utf-8","replace")
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), -1, "", "TIMEOUT"

for arg in sys.argv[1:]:
    qn = int(arg)
    if qn < 1 or qn > len(qs):
        print(f"Q{qn}: out of range"); continue
    raw = qs[qn-1]
    sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", raw)
    print(f"\n=== Q{qn}: {raw[:100]} ===")
    for label, exe in [("sloth", SLOTH), ("duck", DUCK)]:
        times = []
        for trial in range(3):
            dt, rc, out, err = run_once(exe, sql)
            if rc != 0:
                err_line = (err.splitlines() + [""])[0][:120]
                print(f"  {label} trial {trial+1}: ERROR rc={rc} {err_line}")
                times = []
                break
            times.append(dt)
        if times:
            best = min(times)
            print(f"  {label}: best={best*1000:.0f}ms  trials={[f'{t*1000:.0f}' for t in times]}")

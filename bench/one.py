"""Run one ClickBench query 5x on each engine, report min/median.
Usage: python bench/one.py <qnum>
"""
import sys, subprocess, time, os, re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
QFILE = ROOT / "bench/clickbench/queries.sql"
TIMEOUT = 30
qs = [(s[:-1] if s.endswith(";") else s) for s in (l.strip() for l in
      QFILE.read_text(encoding="utf-8").splitlines()) if s and not s.startswith("--")]
qnum = int(sys.argv[1])
sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", qs[qnum - 1])
N = int(sys.argv[2]) if len(sys.argv) > 2 else 5
def time_one(exe, label):
    times = []
    for i in range(N):
        t0 = time.perf_counter()
        try:
            p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT)
            dt = (time.perf_counter() - t0) * 1000
            if p.returncode != 0:
                print(f"  [{label} trial {i}] ERROR: {p.stderr.decode('utf-8', 'replace')[:120]}")
                return None
            times.append(dt)
        except subprocess.TimeoutExpired:
            print(f"  [{label} trial {i}] TIMEOUT >{TIMEOUT}s")
            return None
    times.sort()
    print(f"  {label:<7s}: min={times[0]:>6.0f}ms  med={times[len(times)//2]:>6.0f}ms  max={times[-1]:>6.0f}ms  {[f'{t:.0f}' for t in times]}")
    return times
print(f"Q{qnum}: {qs[qnum-1][:140]}")
sl = time_one(SLOTH, "sloth")
dk = time_one(DUCK, "duck")
if sl and dk:
    ratio = dk[0] / sl[0]
    print(f"  ratio (min/min): {ratio:.3f}x  status: {'WIN' if ratio > 1.0 else 'LOSS'}")

"""Run a single ClickBench query N times against SlothDB and DuckDB.
Usage: python one_query.py <query_index> [trials=5]
30s timeout per trial. Min-of-N reported.
"""
import os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
QFILE = ROOT / "bench/clickbench/queries.sql"
TIMEOUT = 30


def run(exe, sql):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT, env=env)
        return time.perf_counter() - t0, p.returncode, p.stdout.decode("utf-8", "replace")[:500]
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), -1, "TIMEOUT"


def main():
    idx = int(sys.argv[1])
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    qs = [(s[:-1] if s.endswith(";") else s) for s in (l.strip() for l in
          QFILE.read_text(encoding="utf-8").splitlines()) if s and not s.startswith("--")]
    q = qs[idx - 1]
    sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", q)
    print(f"Q{idx}: {q[:120]}")
    s_times, d_times = [], []
    for i in range(trials):
        ds, _, _ = run(SLOTH, sql)
        s_times.append(ds)
        dd, _, _ = run(DUCK, sql)
        d_times.append(dd)
        print(f"  trial {i+1}: sloth={ds*1000:.0f}ms duck={dd*1000:.0f}ms ratio={dd/ds:.2f}x")
    sm = min(s_times); dm = min(d_times)
    print(f"\nMIN: sloth={sm*1000:.0f}ms duck={dm*1000:.0f}ms ratio={dm/sm:.2f}x")


if __name__ == "__main__":
    main()

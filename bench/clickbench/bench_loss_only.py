"""Run only LOSS/WRONG/TIMEOUT queries from queries.sql, min-of-3, 30s per trial.
Prints ratio + DuckDB time so we can rank attack order by closest-to-flip first.
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
        return time.perf_counter() - t0, p.returncode == 0
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), False

def min_of_3(exe, sql):
    best = float("inf"); okany = False
    for _ in range(3):
        dt, ok = run(exe, sql)
        if ok: okany = True
        if dt < best: best = dt
    return best, okany

def main():
    target = set(int(x) for x in sys.argv[1:]) if len(sys.argv) > 1 else None
    qs = [(s[:-1] if s.endswith(";") else s) for s in (l.strip() for l in
          QFILE.read_text(encoding="utf-8").splitlines()) if s and not s.startswith("--")]
    print(f"{'Q':>3} {'SLOTH':>10} {'DUCK':>10} {'RATIO':>8}  STATUS")
    for i, q in enumerate(qs, 1):
        if target and i not in target: continue
        sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", q)
        sd, ok_s = min_of_3(SLOTH, sql)
        dd, ok_d = min_of_3(DUCK, sql)
        ratio = dd / sd if sd > 0 else 0
        if not ok_s:
            st = "S_FAIL"
        elif not ok_d:
            st = "WIN_DF" if ok_s else "BOTH_FAIL"
        elif sd >= TIMEOUT - 0.5:
            st = "TIMEOUT"
        elif ratio >= 1.0:
            st = "WIN"
        else:
            st = "LOSS"
        print(f"{i:>3} {sd*1000:>9.0f}ms {dd*1000:>9.0f}ms {ratio:>7.3f}x  {st}")

if __name__ == "__main__":
    main()

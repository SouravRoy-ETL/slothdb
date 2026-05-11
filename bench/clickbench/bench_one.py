"""Bench a single ClickBench query: SlothDB vs DuckDB, min-of-3, 30s cap.
Usage: python bench_one.py <q_index 1..43>
"""
import os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
QFILE = ROOT / "bench/clickbench/queries.sql"
TIMEOUT = 30

def queries():
    DATA = ROOT / "bench/clickbench/data/hits.parquet"
    out = []
    for ln in QFILE.read_text(encoding="utf-8").splitlines():
        s = ln.strip()
        if not s or s.startswith("--"): continue
        if s.endswith(";"): s = s[:-1]
        # Replace bare `hits` table reference with explicit parquet path,
        # matching verify_all.py.
        s = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", s)
        out.append(s)
    return out

def run(exe, sql):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT, env=env)
        dt = time.perf_counter() - t0
        out = p.stdout.decode("utf-8","replace")
        err = p.stderr.decode("utf-8","replace")
        if p.returncode != 0 or "Error" in err or "Error" in out:
            print(f"  ERROR ({exe}): {(err or out).splitlines()[0][:150] if (err or out) else 'rc='+str(p.returncode)}")
        return dt, out, err, p.returncode
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), "", "TIMEOUT", -1

def best3(exe, sql):
    return min(run(exe, sql)[0] for _ in range(3))

def main():
    if len(sys.argv) < 2:
        print("usage: bench_one.py <q_index 1..43> [trials=3]"); return
    qi = int(sys.argv[1])
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    qs = queries()
    sql = qs[qi-1]
    print(f"Q{qi}: {sql[:160]}")
    # Interleave runs so neither engine gets all-cold or all-warm advantage.
    s_times = []; d_times = []
    for _ in range(trials):
        s_times.append(run(SLOTH, sql)[0])
        d_times.append(run(DUCK,  sql)[0])
    s_best = min(s_times); d_best = min(d_times)
    print(f"SlothDB: {[f'{t*1000:.0f}ms' for t in s_times]}  best={s_best*1000:.0f}ms")
    print(f"DuckDB : {[f'{t*1000:.0f}ms' for t in d_times]}  best={d_best*1000:.0f}ms")
    if s_best < TIMEOUT and d_best < TIMEOUT:
        ratio = d_best / s_best
        verdict = "WIN" if ratio >= 1.0 else "LOSS"
        print(f"Ratio  : {ratio:.3f}x  -> {verdict}")

if __name__ == "__main__":
    main()

"""Test ONE query individually with 30s timeout, min-of-3 trials."""
import subprocess, time, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
TIMEOUT_S = 30

def run(exe, sql, n=3):
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        try:
            p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            return None, "TIMEOUT"
        if p.returncode != 0:
            return None, p.stderr.decode("utf8", errors="replace")[:200]
        times.append(time.perf_counter() - t0)
    return min(times), None

def main(qnum):
    Q = ROOT / "bench/clickbench/queries.sql"
    sqls = [l.rstrip(";").strip() for l in Q.read_text().splitlines()
            if l.strip().upper().startswith("SELECT")]
    sql = sqls[qnum-1].replace(" FROM hits", f" FROM '{DATA}'")
    print(f"Q{qnum}: {sql[:120]}{'...' if len(sql)>120 else ''}")
    sd, se = run(SLOTH, sql)
    dd, de = run(DUCK, sql)
    s_str = f"{sd*1000:.0f}ms" if sd else f"FAIL/{se}"
    d_str = f"{dd*1000:.0f}ms" if dd else f"FAIL/{de}"
    ratio = f"{(dd/sd):.2f}x" if (sd and dd) else "?"
    verdict = "WIN" if (sd and dd and dd/sd > 1.0) else ("LOSS" if (sd and dd) else "?")
    print(f"  sloth={s_str} duck={d_str} ratio={ratio} {verdict}")
    return verdict

if __name__ == "__main__":
    qnum = int(sys.argv[1])
    main(qnum)

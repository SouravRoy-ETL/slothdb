"""Verify + time one ClickBench query: interleaved S/D trials, min-of-N,
plus a sorted-line correctness comparison.
Usage: python bench/vt.py <qid> [trials]   (default 7 trials)
"""
import os, subprocess, sys, time, re
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
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

def run(exe, sql):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT, env=env)
        dt = time.perf_counter() - t0
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        if p.returncode != 0:
            return dt, None, "FAIL rc=%d %s" % (p.returncode, (err or out).strip()[:80])
        for sig in ("Conversion Error", "Binder Error", "Catalog Error", "Parser Error",
                    "IO Error", "Constraint Error", "Out of Memory", "Internal Error"):
            if sig in out or sig in err:
                return dt, None, "FAIL " + sig
        return dt, out, None
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), None, "TIMEOUT"

def norm(out):
    if out is None:
        return None
    lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
    return sorted(lines)

def main():
    qs = queries()
    qid = int(sys.argv[1])
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", qs[qid - 1])
    print(f"Q{qid}: {sql[:110]}{'...' if len(sql) > 110 else ''}")
    sb, db = float("inf"), float("inf")
    s_out, d_out, s_err, d_err = None, None, None, None
    for _ in range(trials):
        st, so, se = run(SLOTH, sql)
        dt, do, de = run(DUCK, sql)
        if se: s_err = se
        if de: d_err = de
        if so is not None: s_out = so
        if do is not None: d_out = do
        sb = min(sb, st); db = min(db, dt)
    ratio = db / sb if sb else 0
    verdict = "WIN" if ratio >= 1.0 else "LOSS"
    if s_err: verdict = "SLOTH " + s_err
    print(f"  SLOTH={sb*1000:.0f}ms  DUCK={db*1000:.0f}ms  ratio={ratio:.3f}x  {verdict}")
    sn, dn = norm(s_out), norm(d_out)
    if s_err or d_err:
        print(f"  correctness: skipped (s_err={s_err} d_err={d_err})")
    elif sn == dn:
        print(f"  correctness: MATCH ({len(sn)} lines)")
    else:
        print(f"  correctness: MISMATCH  sloth={len(sn) if sn else 0} lines  duck={len(dn) if dn else 0} lines")
        for i in range(max(len(sn or []), len(dn or []))):
            a = sn[i] if sn and i < len(sn) else "<none>"
            b = dn[i] if dn and i < len(dn) else "<none>"
            if a != b:
                print(f"    [{i}] sloth: {a[:90]}")
                print(f"    [{i}] duck : {b[:90]}")

if __name__ == "__main__":
    main()

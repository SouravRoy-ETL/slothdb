"""Check all LIKE queries quickly."""
import subprocess, time, os, re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
QFILE = ROOT / "bench/clickbench/queries.sql"
qs = [(s[:-1] if s.endswith(";") else s) for s in (l.strip() for l in
      QFILE.read_text(encoding="utf-8").splitlines()) if s and not s.startswith("--")]
TARGETS = [21, 22, 23, 25, 26, 27, 28]
for i in TARGETS:
    q = qs[i-1]
    sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", q)
    sm, dm = [], []
    for _ in range(3):
        t = time.perf_counter()
        ps = subprocess.run([str(SLOTH), '-c', sql], capture_output=True, timeout=30)
        sm.append(time.perf_counter()-t)
        t = time.perf_counter()
        pd = subprocess.run([str(DUCK), '-c', sql], capture_output=True, timeout=30)
        dm.append(time.perf_counter()-t)
    s, d = min(sm), min(dm)
    print(f'Q{i}: sloth={s*1000:.0f}ms duck={d*1000:.0f}ms ratio={d/s:.2f}x  {"WIN" if d>s else "LOSS"}')

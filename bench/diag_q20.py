"""Diagnose Q20 dict-skip and matching RG decode costs."""
import subprocess, time, os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = str(ROOT / "bench/clickbench/data/hits.parquet").replace("\\","/")

queries = {
    'q20_real':         f"SELECT UserID FROM '{DATA}' WHERE UserID = 435090932899640449",
    'q20_impossible':   f"SELECT UserID FROM '{DATA}' WHERE UserID = 1",
    'q20_count_match':  f"SELECT COUNT(*) FROM '{DATA}' WHERE UserID = 435090932899640449",
    'q20_count_neq':    f"SELECT COUNT(*) FROM '{DATA}' WHERE UserID = 1",
}
for name, sql in queries.items():
    sm, dm = [], []
    for _ in range(3):
        t = time.perf_counter()
        subprocess.run([str(SLOTH), '-c', sql], capture_output=True, timeout=30)
        sm.append(time.perf_counter()-t)
        t = time.perf_counter()
        subprocess.run([str(DUCK), '-c', sql], capture_output=True, timeout=30)
        dm.append(time.perf_counter()-t)
    s, d = min(sm), min(dm)
    print(f'{name:20s} sloth={s*1000:.0f}ms duck={d*1000:.0f}ms ratio={d/s:.2f}x')

"""Diagnose what Q11's wall time goes to: decode vs filter vs DISTINCT layer.

Compares min-of-5 timings on three shape variants:
  Q11_full: WHERE + GROUP BY + COUNT(DISTINCT UserID)
  Q11_count: WHERE + GROUP BY + COUNT(*)         -- no DISTINCT layer
  Q11_filt:  WHERE + COUNT(*)                     -- no GROUP BY layer
"""
import os, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"

QS = {
    "Q11_full":  "SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM 'DATA' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    "Q11_count": "SELECT MobilePhoneModel, COUNT(*) AS u FROM 'DATA' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    "Q11_filt":  "SELECT COUNT(*) FROM 'DATA' WHERE MobilePhoneModel <> ''",
}

def t(exe, sql, n=5):
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=60)
        if p.returncode != 0: return None
        times.append(time.perf_counter() - t0)
    return min(times)

print(f"{'Q':<11}{'sloth':>10}{'duck':>10}{'ratio':>8}")
for q, sql in QS.items():
    s = sql.replace("'DATA'", f"'{DATA}'")
    sd, dd = t(SLOTH, s), t(DUCK, s)
    if sd and dd:
        print(f"{q:<11}{sd*1000:>9.0f}ms{dd*1000:>9.0f}ms{dd/sd:>7.2f}x")

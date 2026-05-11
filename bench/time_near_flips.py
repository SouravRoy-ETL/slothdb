"""Min-of-5 timing for near-flip ClickBench queries: Q11/Q12/Q15/Q21/Q22."""
import os, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"

QUERIES = {
    "Q11": "SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM 'DATA' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    "Q12": "SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM 'DATA' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10",
    "Q15": "SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM 'DATA' WHERE SearchPhrase <> '' GROUP BY SearchEngineID, SearchPhrase ORDER BY c DESC LIMIT 10",
    "Q21": "SELECT COUNT(*) FROM 'DATA' WHERE URL LIKE '%google%'",
    "Q22": "SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM 'DATA' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
}

def time_one(exe, sql, n=5):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=60, env=env)
        dt = time.perf_counter() - t0
        if p.returncode != 0:
            return None
        times.append(dt)
    return min(times)

print(f"{'Q':<5}{'sloth':>10}{'duck':>10}{'ratio':>8}")
for q, sql in QUERIES.items():
    s = sql.replace("'DATA'", f"'{DATA}'")
    sd = time_one(SLOTH, s)
    dd = time_one(DUCK, s)
    if sd and dd:
        print(f"{q:<5}{sd*1000:>9.0f}ms{dd*1000:>9.0f}ms{dd/sd:>7.2f}x")
    else:
        print(f"{q:<5} fail")

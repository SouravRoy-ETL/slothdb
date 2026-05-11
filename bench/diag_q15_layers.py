"""Q15 component decomposition: filter, single-col GROUP BY, 2-col GROUP BY."""
import subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"

QS = {
    "Q15_full": "SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM 'DATA' WHERE SearchPhrase <> '' GROUP BY SearchEngineID, SearchPhrase ORDER BY c DESC LIMIT 10",
    "Q15_filt": "SELECT COUNT(*) FROM 'DATA' WHERE SearchPhrase <> ''",
    "Q15_1col": "SELECT SearchPhrase, COUNT(*) AS c FROM 'DATA' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
    "Q15_int":  "SELECT SearchEngineID, COUNT(*) AS c FROM 'DATA' WHERE SearchPhrase <> '' GROUP BY SearchEngineID ORDER BY c DESC LIMIT 10",
    "Q15_1nf":  "SELECT SearchPhrase, COUNT(*) AS c FROM 'DATA' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
    "Q15_1d":   "SELECT COUNT(DISTINCT SearchPhrase) FROM 'DATA'",
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

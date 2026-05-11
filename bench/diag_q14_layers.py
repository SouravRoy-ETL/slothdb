"""Q14 component decomposition."""
import subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"

QS = {
    "Q14_full": "SELECT SearchPhrase, COUNT(DISTINCT UserID) AS u FROM 'DATA' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY u DESC LIMIT 10",
    "Q14_count":"SELECT SearchPhrase, COUNT(*) AS u FROM 'DATA' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY u DESC LIMIT 10",
    "Q14_filt": "SELECT COUNT(*) FROM 'DATA' WHERE SearchPhrase <> ''",
    "Q14_scan": "SELECT COUNT(*) FROM 'DATA'",
}

def t(exe, sql, n=4):
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=120)
        if p.returncode != 0: return None
        times.append(time.perf_counter() - t0)
    return min(times)

print(f"{'Q':<11}{'sloth':>10}{'duck':>10}{'ratio':>8}")
for q, sql in QS.items():
    s = sql.replace("'DATA'", f"'{DATA}'")
    sd, dd = t(SLOTH, s), t(DUCK, s)
    if sd and dd:
        print(f"{q:<11}{sd*1000:>9.0f}ms{dd*1000:>9.0f}ms{dd/sd:>7.2f}x")

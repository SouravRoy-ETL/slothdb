"""Q20 layer diagnostics."""
import subprocess, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
QS = {
    "Q20_full": "SELECT UserID FROM 'DATA' WHERE UserID = 435090932899640449",
    "Q20_count": "SELECT COUNT(*) FROM 'DATA' WHERE UserID = 435090932899640449",
    "Q20_decode_only": "SELECT MIN(UserID) FROM 'DATA' WHERE UserID = 435090932899640449",
    "Q20_no_filter": "SELECT MIN(UserID) FROM 'DATA'",
}
def t(exe, sql, n=4):
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=30)
        if p.returncode != 0: return None
        times.append(time.perf_counter() - t0)
    return min(times)
print(f"{'Q':<22}{'sloth':>10}{'duck':>10}{'ratio':>8}")
for q, sql in QS.items():
    s = sql.replace("'DATA'", f"'{DATA}'")
    sd, dd = t(SLOTH, s), t(DUCK, s)
    if sd and dd:
        print(f"{q:<22}{sd*1000:>9.0f}ms{dd*1000:>9.0f}ms{dd/sd:>7.2f}x")

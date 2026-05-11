"""Min-of-7 timing for unrelated-but-icache-sensitive queries: Q3, Q4, Q5, Q7, Q8.
None of these use the gstr×int distinct path; if they shift, it's pure I-cache layout."""
import subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"

QS = {
    "Q1":  "SELECT COUNT(*) FROM 'DATA'",
    "Q3":  "SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM 'DATA'",
    "Q4":  "SELECT AVG(UserID) FROM 'DATA'",
    "Q5":  "SELECT COUNT(DISTINCT UserID) FROM 'DATA'",
    "Q7":  "SELECT MIN(EventDate), MAX(EventDate) FROM 'DATA'",
    "Q8":  "SELECT AdvEngineID, COUNT(*) FROM 'DATA' WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC",
    "Q9":  "SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM 'DATA' GROUP BY RegionID ORDER BY u DESC LIMIT 10",
    "Q11": "SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM 'DATA' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    "Q30": "SELECT SUM(ResolutionWidth), SUM(ResolutionWidth + 1), SUM(ResolutionWidth + 2), SUM(ResolutionWidth + 3) FROM 'DATA'",
}

def t(exe, sql, n=7):
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=60)
        if p.returncode != 0: return None
        times.append(time.perf_counter() - t0)
    return min(times)

print(f"{'Q':<5}{'sloth_min':>11}{'sloth_med':>11}")
for q, sql in QS.items():
    s = sql.replace("'DATA'", f"'{DATA}'")
    times = []
    for _ in range(7):
        t0 = time.perf_counter()
        p = subprocess.run([str(SLOTH), "-c", s], capture_output=True, timeout=60)
        if p.returncode != 0: continue
        times.append(time.perf_counter() - t0)
    if times:
        times.sort()
        med = times[len(times)//2]
        print(f"{q:<5}{times[0]*1000:>10.0f}ms{med*1000:>10.0f}ms")

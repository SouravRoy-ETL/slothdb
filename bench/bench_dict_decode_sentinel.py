"""Sentinel verification for batched VARCHAR DICT decode.
Runs Q1/Q5/Q7/Q8 (sentinels per Q35 memo) + Q11/Q12/Q21 (targets) with N=3.
Reports min/median per engine and ratio. Sentinels must stay within ±10%
of last-known-good; targets should ideally improve.
"""
import os, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
N = 3

# Selected queries by 1-based index in queries.sql.
QUERIES = {
    1:  "SELECT COUNT(*) FROM hits",
    5:  "SELECT COUNT(DISTINCT UserID) FROM hits",
    7:  "SELECT MIN(EventDate), MAX(EventDate) FROM hits",
    8:  "SELECT AdvEngineID, COUNT(*) FROM hits WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC",
    11: "SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    12: "SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM hits WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10",
    21: "SELECT COUNT(*) FROM hits WHERE URL LIKE '%google%'",
    28: "SELECT CounterID, AVG(STRLEN(URL)) AS l, COUNT(*) AS c FROM hits WHERE URL <> '' GROUP BY CounterID HAVING COUNT(*) > 100000 ORDER BY l DESC LIMIT 25",
}

def run(exe, sql):
    sql = sql.replace("hits", f"'{DATA}'")
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.perf_counter()
    p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=60, env=env)
    return time.perf_counter() - t0, p.returncode

def measure(label, exe, sql):
    times = []
    for _ in range(N):
        dt, rc = run(exe, sql)
        if rc != 0:
            return None
        times.append(dt)
    times.sort()
    return times

print(f"# Q     | sloth ms       | duck ms        | ratio  | verdict")
print(f"# ------+----------------+----------------+--------+--------")
for qid, sql in QUERIES.items():
    s = measure("sloth", SLOTH, sql)
    d = measure("duck", DUCK, sql)
    if s is None or d is None:
        print(f"  {qid:>3}  | FAILED")
        continue
    s_min = s[0]*1000; d_min = d[0]*1000
    s_med = s[N//2]*1000; d_med = d[N//2]*1000
    ratio = d[0]/s[0]
    verdict = "WIN" if ratio > 1.0 else "LOSS"
    print(f"  {qid:>3}  | min={s_min:>5.0f} med={s_med:>5.0f} | min={d_min:>5.0f} med={d_med:>5.0f} | {ratio:.2f}x | {verdict}")

"""Q27 diagnostic: time component parts.
Q27 = SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*), COUNT(DISTINCT UserID)
     FROM hits WHERE Title LIKE %Google% AND URL NOT LIKE %.google.% AND SearchPhrase<>''
     GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10
"""
import os, subprocess, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"

QSUITES = [
    ("Q27-full",
     f"SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, COUNT(DISTINCT UserID) FROM '{DATA}' WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"),
    ("Q27-noLIKE",
     f"SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, COUNT(DISTINCT UserID) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"),
    ("Q27-noDISTINCT",
     f"SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c FROM '{DATA}' WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"),
    ("Q27-onlyCOUNT",
     f"SELECT SearchPhrase, COUNT(*) AS c FROM '{DATA}' WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"),
    ("Q27-justFilters",
     f"SELECT COUNT(*) FROM '{DATA}' WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' AND SearchPhrase <> ''"),
]

def run(exe, sql, n=3):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    best = 99999
    for _ in range(n):
        t0 = time.perf_counter()
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=30, env=env)
        dt = (time.perf_counter() - t0) * 1000
        if p.returncode != 0:
            return -1
        if dt < best:
            best = dt
    return best

for name, sql in QSUITES:
    s = run(SLOTH, sql)
    d = run(DUCK, sql)
    ratio = d/s if (s > 0 and d > 0) else 0
    print(f"{name:20s} SLOTH={s:5.0f}ms  DUCK={d:5.0f}ms  ratio={ratio:.2f}x")

"""Why is Q35 flagged WRONG by verify_all.norm()?"""
import sys, re, subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0] / "clickbench"))
from verify_all import norm

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
SQL = f"SELECT 1, URL, COUNT(*) AS c FROM '{DATA}' GROUP BY 1, URL ORDER BY c DESC LIMIT 10"

so = subprocess.run([str(SLOTH), "-c", SQL], capture_output=True, timeout=60).stdout.decode("utf-8","replace")
do = subprocess.run([str(DUCK), "-c", SQL], capture_output=True, timeout=60).stdout.decode("utf-8","replace")

ns = norm(so); nd = norm(do)
print("MATCH" if ns == nd else "MISMATCH")
sl = ns.split("\n"); dl = nd.split("\n")
print(f"sloth tokens: {len(sl)}; duck tokens: {len(dl)}")
from collections import Counter
sc, dc = Counter(sl), Counter(dl)
all_keys = sc.keys() | dc.keys()
for k in sorted(all_keys):
    if sc[k] != dc[k]:
        print(f"  s={sc[k]} d={dc[k]} {k!r}")

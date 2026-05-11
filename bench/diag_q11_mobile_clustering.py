"""How clustered is MobilePhoneModel in hits.parquet?
If consecutive rows mostly share the same model, the per-set state is L1/L2 resident
and prefetching slots ahead won't help. If it's random, prefetching might."""
import os, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"

# Get a sample: read 1M rows of MobilePhoneModel where non-empty.
sql = f"SELECT MobilePhoneModel FROM '{DATA}' WHERE MobilePhoneModel <> '' LIMIT 1000000"
p = subprocess.run([str(SLOTH), "-c", sql], capture_output=True, timeout=60)
out = p.stdout.decode("utf-8", "replace")

# Count run lengths.
lines = [l.strip() for l in out.splitlines() if l.strip() and "MobilePhoneModel" not in l and "rows" not in l and not l.startswith("─") and not l.startswith("┌") and not l.startswith("└")]
# Strip box-drawing
clean = []
for l in lines:
    s = "".join(c for c in l if c not in "│┌┐└┘─┬┴├┤┼")
    s = s.strip()
    if s and s != "MobilePhoneModel":
        clean.append(s)

# Distinct + run length
from collections import Counter
distinct = len(set(clean))
runs = 1
for i in range(1, len(clean)):
    if clean[i] != clean[i-1]: runs += 1
top10 = Counter(clean).most_common(10)

print(f"Sampled rows: {len(clean)}")
print(f"Distinct values: {distinct}")
print(f"Runs (groups of consecutive equal): {runs}")
print(f"Avg run length: {len(clean)/runs:.1f}")
print("Top-5 frequencies:", top10[:5])

"""Print dict_page sizes for SearchPhrase, MobilePhoneModel, RegionID columns
(to design a cardinality gate for Q14 2-stage)."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"

# Use DuckDB to read parquet metadata
sql = f"""SELECT row_group_id, path_in_schema, total_uncompressed_size, total_compressed_size, num_values, encodings FROM parquet_metadata('{DATA}') WHERE path_in_schema IN ('SearchPhrase', 'MobilePhoneModel', 'RegionID', 'UserID', 'URL', 'Title') ORDER BY path_in_schema, row_group_id"""
p = subprocess.run([str(DUCK), "-c", sql], capture_output=True, timeout=30)
out = p.stdout.decode("utf-8","replace")
lines = out.splitlines()
# Aggregate by column name
from collections import defaultdict
totals_uncomp = defaultdict(int)
totals_comp = defaultdict(int)
totals_rows = defaultdict(int)
for l in lines:
    parts = l.split("│")
    parts = [p.strip() for p in parts if p.strip()]
    if len(parts) < 5: continue
    if parts[0].isdigit() == False and not parts[0]: continue
    try:
        rg = parts[0]
        col = parts[1]
        unc = int(parts[2])
        cmp = int(parts[3])
        nv  = int(parts[4])
        if col in ("SearchPhrase","MobilePhoneModel","RegionID","UserID","URL","Title"):
            totals_uncomp[col] += unc
            totals_comp[col] += cmp
            totals_rows[col] += nv
    except: pass

print(f"{'Column':<20}{'uncomp_MB':>12}{'comp_MB':>12}{'rows_M':>10}")
for c in sorted(totals_uncomp.keys()):
    print(f"{c:<20}{totals_uncomp[c]/1e6:>11.1f}{totals_comp[c]/1e6:>11.1f}{totals_rows[c]/1e6:>9.1f}")

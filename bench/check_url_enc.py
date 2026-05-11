"""Probe URL column encoding via SlothDB."""
import subprocess

DATA = r'C:/Users/soura/Documents/lightdb/bench/clickbench/data/hits.parquet'
SLOTH = r'C:/Users/soura/Documents/lightdb/build/src/Release/slothdb.exe'

# Use parquet_metadata if available, else just count distinct
sql = f"SELECT COUNT(*), COUNT(DISTINCT URL) FROM '{DATA}'"
print(sql)
import time
t0 = time.perf_counter()
p = subprocess.run([SLOTH, "-c", sql], capture_output=True, timeout=60)
dt = time.perf_counter() - t0
print(f"{dt*1000:.0f}ms")
print(p.stdout.decode("utf-8", "replace")[:500])

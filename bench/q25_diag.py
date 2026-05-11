"""Q25 component breakdown."""
import subprocess, time
from pathlib import Path
SLOTH = str((Path(__file__).resolve().parents[1] / 'build/src/Release/slothdb.exe').resolve())
DUCK = str((Path(__file__).resolve().parents[1] / 'real-life-testing/duckdb.exe').resolve())
DATA = "bench/clickbench/data/hits.parquet"
queries = {
    'Q25 with filter': f"SELECT SearchPhrase FROM '{DATA}' WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10",
    'Q25 no-filter':   f"SELECT SearchPhrase FROM '{DATA}' ORDER BY EventTime LIMIT 10",
    'EventTime min only':  f"SELECT EventTime FROM '{DATA}' ORDER BY EventTime LIMIT 10",
    'EventTime no proj':   f"SELECT EventTime FROM '{DATA}' WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10",
}
print('===== SLOTH =====')
for name, sql in queries.items():
    t0 = time.perf_counter()
    p = subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=30)
    dt = (time.perf_counter() - t0)*1000
    if p.returncode == 0:
        print(f'{name:<26s}: {dt:>6.0f}ms')
    else:
        print(f'{name:<26s}: ERROR {p.stderr.decode()[:120]}')
print('===== DUCK =====')
for name, sql in queries.items():
    t0 = time.perf_counter()
    p = subprocess.run([DUCK, '-c', sql], capture_output=True, timeout=30)
    dt = (time.perf_counter() - t0)*1000
    if p.returncode == 0:
        print(f'{name:<26s}: {dt:>6.0f}ms')
    else:
        print(f'{name:<26s}: ERROR {p.stderr.decode()[:120]}')

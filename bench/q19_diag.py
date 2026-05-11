"""Q19 component breakdown."""
import subprocess, time
from pathlib import Path
SLOTH = str((Path(__file__).resolve().parents[1] / 'build/src/Release/slothdb.exe').resolve())
DATA = "bench/clickbench/data/hits.parquet"
queries = {
    'EXTRACT min/max': f"SELECT MIN(extract(minute FROM EventTime)), MAX(extract(minute FROM EventTime)) FROM '{DATA}'",
    'EXTRACT no group by': f"SELECT extract(minute FROM EventTime) AS m FROM '{DATA}' LIMIT 5",
    'EXTRACT GROUP BY m only': f"SELECT extract(minute FROM EventTime) AS m, COUNT(*) FROM '{DATA}' GROUP BY m",
    'GROUP BY UserID, m only': f"SELECT UserID, extract(minute FROM EventTime) AS m, COUNT(*) FROM '{DATA}' GROUP BY UserID, m LIMIT 5",
    'Q19 full': f"SELECT UserID, extract(minute FROM EventTime) AS m, SearchPhrase, COUNT(*) FROM '{DATA}' GROUP BY UserID, m, SearchPhrase ORDER BY COUNT(*) DESC LIMIT 10",
}
for name, sql in queries.items():
    t0 = time.perf_counter()
    try:
        p = subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=30)
        dt = (time.perf_counter() - t0)*1000
        if p.returncode == 0:
            print(f'{name:<40s}: {dt:>6.0f}ms')
        else:
            print(f'{name:<40s}: ERROR {p.stderr.decode()[:120]}')
    except subprocess.TimeoutExpired:
        print(f'{name:<40s}: TIMEOUT')

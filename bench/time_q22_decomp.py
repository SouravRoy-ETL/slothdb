"""Decompose Q22 to find the actual bottleneck."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

queries = [
    ('Q22-full',           f"SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '{DATA}' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"),
    ('Q22-no-MIN-URL',     f"SELECT SearchPhrase, COUNT(*) AS c FROM '{DATA}' WHERE URL LIKE '%google%' AND SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"),
    ('Q22-no-NE',          f"SELECT SearchPhrase, MIN(URL), COUNT(*) AS c FROM '{DATA}' WHERE URL LIKE '%google%' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10"),
    ('Q21-only-LIKE',      f"SELECT COUNT(*) FROM '{DATA}' WHERE URL LIKE '%google%'"),
    ('count-only-NE',      f"SELECT COUNT(*) FROM '{DATA}' WHERE SearchPhrase <> ''"),
    ('count-URL',          f"SELECT COUNT(URL) FROM '{DATA}'"),
    ('count-SearchPhrase', f"SELECT COUNT(SearchPhrase) FROM '{DATA}'"),
    ('count-both',         f"SELECT COUNT(URL), COUNT(SearchPhrase) FROM '{DATA}'"),
    ('avg-strlen-URL',     f"SELECT AVG(STRLEN(URL)) FROM '{DATA}' WHERE URL <> ''"),
    ('Q21-no-filter',      f"SELECT COUNT(*) FROM '{DATA}'"),
]

for label, sql in queries:
    times = []
    try:
        for _ in range(3):
            t = time.perf_counter()
            r = subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=15)
            times.append(time.perf_counter() - t)
        sloth = min(times) * 1000
        print(f'{label:<25} {sloth:>7.0f}ms', flush=True)
    except subprocess.TimeoutExpired:
        print(f'{label:<25} TIMEOUT', flush=True)

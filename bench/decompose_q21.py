import subprocess, time, os
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\','/')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')

queries = {
    'count_no_filter':       f"SELECT COUNT(*) FROM '{DATA}'",
    'count_url_neq_empty':   f"SELECT COUNT(*) FROM '{DATA}' WHERE URL <> ''",
    'count_url_like_google': f"SELECT COUNT(*) FROM '{DATA}' WHERE URL LIKE '%google%'",
}
for name, sql in queries.items():
    sm, dm = [], []
    for _ in range(3):
        t = time.perf_counter()
        subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=30)
        sm.append(time.perf_counter()-t)
        t = time.perf_counter()
        subprocess.run([DUCK, '-c', sql], capture_output=True, timeout=30)
        dm.append(time.perf_counter()-t)
    print(f'{name:30s} sloth={min(sm)*1000:.0f}ms duck={min(dm)*1000:.0f}ms ratio={min(dm)/min(sm):.2f}x')

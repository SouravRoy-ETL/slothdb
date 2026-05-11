import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')
SQL = f"SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT UserID) FROM '{DATA}' GROUP BY RegionID ORDER BY c DESC LIMIT 10"
for engine, exe in [('SlothDB', SLOTH), ('DuckDB', DUCK)]:
    times = []
    for _ in range(5):
        t = time.perf_counter()
        subprocess.run([exe, '-c', SQL], capture_output=True, timeout=60)
        times.append(time.perf_counter() - t)
    print(engine, [f'{t*1000:.0f}' for t in times], 'min', f'{min(times)*1000:.0f}ms')

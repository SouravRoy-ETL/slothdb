import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

queries = {
    11: f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{DATA}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    12: f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{DATA}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10",
}

print('='*60, flush=True)
for q, sql in queries.items():
    s_times = []
    for _ in range(7):
        t = time.perf_counter()
        subprocess.run([SLOTH, '-c', sql], capture_output=True, timeout=60)
        s_times.append(time.perf_counter() - t)
    sloth_times = sorted([t*1000 for t in s_times])
    print(f'Q{q} sloth: {[f"{t:.0f}" for t in sloth_times]} min={sloth_times[0]:.0f} med={sloth_times[3]:.0f}', flush=True)

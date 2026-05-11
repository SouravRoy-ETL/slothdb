import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe = os.path.abspath('build/src/Release/slothdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

queries = {
    'Q11': f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    'Q12': f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10",
}

run_set = sys.argv[1:] if len(sys.argv) > 1 else list(queries.keys())

for name in run_set:
    if name not in queries: continue
    sql = queries[name]
    for _ in range(3):
        subprocess.run([exe, '-c', sql], capture_output=True, timeout=180)
    t = []
    for _ in range(7):
        t0 = time.perf_counter()
        subprocess.run([exe, '-c', sql], capture_output=True, timeout=180)
        t.append(time.perf_counter()-t0)
    t.sort()
    med = t[3]*1000
    print(f'{name}: med={med:.0f}ms  runs={[round(x*1000) for x in t]}')
    sys.stdout.flush()

import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe = os.path.abspath('build/src/Release/slothdb.exe')
duck = os.path.abspath('real-life-testing/duckdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

q11 = f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10"
q12 = f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10"

for name, sql in [('Q11', q11), ('Q12', q12)]:
    for engine, bin in [('slothdb', exe), ('duckdb', duck)]:
        # 2 warmups
        for _ in range(2):
            subprocess.run([bin, '-c', sql], capture_output=True, timeout=60)
        times = []
        for _ in range(5):
            t0 = time.perf_counter()
            subprocess.run([bin, '-c', sql], capture_output=True, timeout=60)
            times.append(time.perf_counter() - t0)
        times.sort()
        med = times[2] * 1000
        print(f'{name} {engine}: med={med:.0f}ms  runs={[round(t*1000) for t in times]}')
        sys.stdout.flush()

import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe_baseline = os.path.abspath('bench_tmp/slothdb_baseline.exe')
exe_composite = os.path.abspath('bench_tmp/slothdb_composite.exe')
duck = os.path.abspath('real-life-testing/duckdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

q11 = f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10"
q12 = f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10"

# Interleave 3-way: baseline, composite, duckdb. Warm each first, then 7 timed runs.
def warm(bin, sql, n=2):
    for _ in range(n):
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=180)

for name, sql in [('Q11', q11), ('Q12', q12)]:
    warm(exe_baseline, sql); warm(exe_composite, sql); warm(duck, sql)
    a_t = []; b_t = []; d_t = []
    for _ in range(7):
        t0 = time.perf_counter()
        subprocess.run([exe_baseline, '-c', sql], capture_output=True, timeout=180)
        a_t.append(time.perf_counter()-t0)
        t0 = time.perf_counter()
        subprocess.run([exe_composite, '-c', sql], capture_output=True, timeout=180)
        b_t.append(time.perf_counter()-t0)
        t0 = time.perf_counter()
        subprocess.run([duck, '-c', sql], capture_output=True, timeout=180)
        d_t.append(time.perf_counter()-t0)
    a_t.sort(); b_t.sort(); d_t.sort()
    am = a_t[3]*1000; bm = b_t[3]*1000; dm = d_t[3]*1000
    print(f'{name}: baseline med={am:.0f}ms  composite med={bm:.0f}ms  duck med={dm:.0f}ms')
    print(f'  composite vs baseline: {(bm-am):+.0f}ms ({(bm/am-1)*100:+.1f}%)')
    print(f'  composite ratio (duck/composite): {dm/bm:.2f}x  (need >= 1.0 to win)')
    print(f'  baseline runs:  {[round(t*1000) for t in a_t]}')
    print(f'  composite runs: {[round(t*1000) for t in b_t]}')
    print(f'  duck runs:      {[round(t*1000) for t in d_t]}')
    sys.stdout.flush()

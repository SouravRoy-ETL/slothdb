import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe_a = os.path.abspath('bench_tmp/slothdb_baseline.exe')
exe_b = os.path.abspath('bench_tmp/slothdb_composite.exe')
duck = os.path.abspath('real-life-testing/duckdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

q11 = f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10"
q12 = f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10"

def time_query(bin, sql, n=5):
    # 2 warmups
    for _ in range(2):
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=120)
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=120)
        times.append(time.perf_counter() - t0)
    times.sort()
    return times[n//2] * 1000, [round(t*1000) for t in times]

# Interleaved A/B
import itertools
for name, sql in [('Q11', q11), ('Q12', q12)]:
    a_times = []
    b_times = []
    # Round-robin to mitigate system load drift
    for _ in range(5):
        # baseline
        subprocess.run([exe_a, '-c', sql], capture_output=True, timeout=120)
        t0 = time.perf_counter()
        subprocess.run([exe_a, '-c', sql], capture_output=True, timeout=120)
        a_times.append(time.perf_counter() - t0)
        # composite
        subprocess.run([exe_b, '-c', sql], capture_output=True, timeout=120)
        t0 = time.perf_counter()
        subprocess.run([exe_b, '-c', sql], capture_output=True, timeout=120)
        b_times.append(time.perf_counter() - t0)
    a_times.sort(); b_times.sort()
    a_med = a_times[2]*1000; b_med = b_times[2]*1000
    print(f'{name}: baseline_med={a_med:.0f}ms  composite_med={b_med:.0f}ms  delta={(b_med-a_med):+.0f}ms ({(b_med/a_med-1)*100:+.1f}%)')
    print(f'  baseline runs:  {[round(t*1000) for t in a_times]}')
    print(f'  composite runs: {[round(t*1000) for t in b_times]}')
    sys.stdout.flush()

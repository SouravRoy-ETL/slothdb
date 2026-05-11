import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe_baseline = os.path.abspath('bench_tmp/slothdb_baseline.exe')
exe_composite = os.path.abspath('bench_tmp/slothdb_composite.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

q24 = f"SELECT UserID FROM '{data}' WHERE UserID = 435090932899640449"

def time_query(bin, sql, n=5):
    for _ in range(2):
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=180)
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=180)
        times.append(time.perf_counter() - t0)
    times.sort()
    return times, times[n//2]*1000

# Interleave A/B
a_t = []
b_t = []
for _ in range(7):
    subprocess.run([exe_baseline, '-c', q24], capture_output=True, timeout=180)
    t0 = time.perf_counter()
    subprocess.run([exe_baseline, '-c', q24], capture_output=True, timeout=180)
    a_t.append(time.perf_counter()-t0)
    subprocess.run([exe_composite, '-c', q24], capture_output=True, timeout=180)
    t0 = time.perf_counter()
    subprocess.run([exe_composite, '-c', q24], capture_output=True, timeout=180)
    b_t.append(time.perf_counter()-t0)

a_t.sort(); b_t.sort()
a_med = a_t[3]*1000; b_med = b_t[3]*1000
print(f'Q24 baseline med={a_med:.0f}ms  runs={[round(t*1000) for t in a_t]}')
print(f'Q24 composite med={b_med:.0f}ms runs={[round(t*1000) for t in b_t]}')
print(f'delta: {(b_med-a_med):+.0f}ms ({(b_med/a_med-1)*100:+.1f}%)')

import subprocess, time, os, sys, re
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe_baseline = os.path.abspath('bench_tmp/slothdb_baseline.exe')
exe_composite = os.path.abspath('bench_tmp/slothdb_composite.exe')
duck = os.path.abspath('real-life-testing/duckdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

with open('bench/clickbench/queries.sql') as f:
    qs = [l.strip().rstrip(';') for l in f if l.strip() and not l.strip().startswith('--')]

target_idx = {'Q1':0,'Q5':4,'Q6':5,'Q7':6,'Q9':8,'Q10':9,'Q11':10,'Q12':11,'Q24':23,'Q30':29}

def sub(q):
    return re.sub(r'\bhits\b', "'" + data.replace('\\','\\\\') + "'", q)

def time_query(bin, sql, n=5):
    # 2 warmups
    for _ in range(2):
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=180)
    times = []
    for _ in range(n):
        t0 = time.perf_counter()
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=180)
        times.append(time.perf_counter() - t0)
    times.sort()
    return times[n//2] * 1000

print(f'{"name":>4} {"baseline":>10} {"composite":>10} {"duck":>10} {"b_ratio":>10} {"c_ratio":>10}')
for name, idx in target_idx.items():
    sql = sub(qs[idx])
    b = time_query(exe_baseline, sql, 5)
    c = time_query(exe_composite, sql, 5)
    d = time_query(duck, sql, 5)
    print(f'{name:>4} {b:>10.0f} {c:>10.0f} {d:>10.0f} {d/b:>9.2f}x {d/c:>9.2f}x')
    sys.stdout.flush()

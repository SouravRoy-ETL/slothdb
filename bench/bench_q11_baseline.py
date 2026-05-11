import subprocess, time, os, re, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe = os.path.abspath('build/src/Release/slothdb.exe')
duck = os.path.abspath('real-life-testing/duckdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

with open('bench/clickbench/queries.sql') as f:
    qs = [l.strip().rstrip(';') for l in f if l.strip() and not l.strip().startswith('--')]
target_idx = {'Q1':0,'Q5':4,'Q6':5,'Q7':6,'Q9':8,'Q10':9,'Q11':10,'Q12':11,'Q24':23,'Q30':29}

def sub(q):
    return re.sub(r'\bhits\b', "'" + data.replace('\\','\\\\') + "'", q)

print(f'{"name":>4} {"sloth_ms":>10} {"duck_ms":>10} {"ratio":>8}')
for name, idx in target_idx.items():
    sql = sub(qs[idx])
    row = {}
    for engine, bin in [('slothdb', exe), ('duckdb', duck)]:
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=60)
        times = []
        for _ in range(3):
            t0 = time.perf_counter()
            r = subprocess.run([bin, '-c', sql], capture_output=True, timeout=60)
            times.append(time.perf_counter() - t0)
        times.sort()
        row[engine] = times[1] * 1000
    ratio = row['duckdb']/row['slothdb']
    print(f'{name:>4} {row["slothdb"]:>10.0f} {row["duckdb"]:>10.0f} {ratio:>8.2f}x')
    sys.stdout.flush()

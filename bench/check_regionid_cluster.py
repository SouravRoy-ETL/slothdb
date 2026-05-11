import subprocess, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

# Read first 10K rows and count consecutive-same-value runs.
SQL = f"SELECT RegionID FROM '{DATA}' LIMIT 100000"
p = subprocess.run([SLOTH, '-c', SQL], capture_output=True, timeout=60)
out = p.stdout.decode('utf-8', 'replace').splitlines()
# Skip header lines (look for first integer).
vals = []
for line in out:
    s = line.strip().lstrip('|').rstrip('|').strip()
    try:
        vals.append(int(s))
    except ValueError:
        continue
print(f'Read {len(vals)} values')
if not vals:
    print('No values parsed')
else:
    runs = 1
    for i in range(1, len(vals)):
        if vals[i] != vals[i-1]:
            runs += 1
    avg_run = len(vals) / runs
    unique = len(set(vals))
    print(f'unique={unique} runs={runs} avg_run_length={avg_run:.2f}')
    print(f'cache-last hit rate would be ~{(1 - runs/len(vals)) * 100:.1f}%')

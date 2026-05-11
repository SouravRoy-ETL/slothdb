"""Q11 correctness + timing check post dict-bulk patch."""
import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
D = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')

sql = f"SELECT SearchPhrase, COUNT(*) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY 2 DESC LIMIT 10"

# correctness
ps = subprocess.run([S, '-c', sql], capture_output=True, timeout=60)
pd = subprocess.run([D, '-c', sql], capture_output=True, timeout=60)
sys.stdout.buffer.write(b'--- sloth ---\n'); sys.stdout.buffer.write(ps.stdout); sys.stdout.buffer.write(b'\n')
sys.stdout.buffer.write(b'--- duck ---\n'); sys.stdout.buffer.write(pd.stdout); sys.stdout.buffer.write(b'\n')

# timing 5 trials each, min
def time_n(exe, n):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        subprocess.run([exe, '-c', sql], capture_output=True, timeout=60)
        times.append(time.perf_counter() - t)
    return min(times)

s = time_n(S, 5)
d = time_n(D, 5)
print(f'Q11 sloth={s*1000:.0f}ms duck={d*1000:.0f}ms ratio={d/s:.3f}x')

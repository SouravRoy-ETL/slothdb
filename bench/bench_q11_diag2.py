import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe = os.path.abspath('build/src/Release/slothdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

# How much does pure UserID read+sum cost?
qs = {
    'CNT_STAR':      f"SELECT COUNT(*) FROM '{data}'",
    'USERID_SUM':    f"SELECT SUM(UserID) FROM '{data}'",  # pure decode
    'USERID_MIN':    f"SELECT MIN(UserID) FROM '{data}'",  # uses parquet stats
    'MPM_FILT_CNT':  f"SELECT COUNT(*) FROM '{data}' WHERE MobilePhoneModel <> ''",
}
for name, sql in qs.items():
    for _ in range(2):
        subprocess.run([exe, '-c', sql], capture_output=True, timeout=180)
    t = []
    for _ in range(3):
        t0 = time.perf_counter()
        subprocess.run([exe, '-c', sql], capture_output=True, timeout=180)
        t.append(time.perf_counter()-t0)
    t.sort()
    print(f'{name}: med={t[1]*1000:.0f}ms  runs={[round(x*1000) for x in t]}')
    sys.stdout.flush()

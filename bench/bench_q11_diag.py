import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe = os.path.abspath('build/src/Release/slothdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

q_filter_only = f"SELECT COUNT(*) FROM '{data}' WHERE MobilePhoneModel <> ''"
q_userid_dist = f"SELECT COUNT(DISTINCT UserID) FROM '{data}'"
q_userid_filt = f"SELECT COUNT(DISTINCT UserID) FROM '{data}' WHERE MobilePhoneModel <> ''"
q_grp_only = f"SELECT MobilePhoneModel, COUNT(*) FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY 2 DESC LIMIT 10"
q11 = f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10"

for name, sql in [('FILTER_ONLY', q_filter_only),
                  ('USERID_DIST', q_userid_dist),
                  ('USERID_DIST_FILT', q_userid_filt),
                  ('GRP_ONLY', q_grp_only),
                  ('Q11', q11)]:
    # warm
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

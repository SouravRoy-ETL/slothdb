"""A/B Q11_base + Q13 with skip-group-col patch ON vs OFF."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
S = os.path.abspath('build/src/Release/slothdb.exe')
P = os.path.abspath('bench/clickbench/data/hits.parquet').replace(os.sep, '/')

queries = {
    'Q11_base':  f"SELECT MobilePhoneModel, COUNT(*) AS c FROM '{P}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY c DESC LIMIT 10",
    'Q13_SP':    f"SELECT SearchPhrase, COUNT(*) AS c FROM '{P}' WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10",
}

def time_n(env, sql, n=4):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        try: subprocess.run([S, '-c', sql], capture_output=True, timeout=120, env=env)
        except Exception: return None
        times.append(time.perf_counter() - t)
    return times

env_on = dict(os.environ)
env_off = dict(os.environ); env_off['SLOTH_NO_GROUP_SKIP'] = '1'

print(f"{'Q':10} {'skip_ON_ms':>12} {'skip_OFF_ms':>14} {'delta':>10}")
for name, sql in queries.items():
    on = time_n(env_on, sql); off = time_n(env_off, sql)
    on_min = min(on) * 1000; off_min = min(off) * 1000
    delta = on_min - off_min
    print(f"{name:10} {on_min:12.0f} {off_min:14.0f} {delta:+10.0f}")

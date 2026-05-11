"""Run Q32 multiple times in same path; check whether output is deterministic."""
import subprocess, os, sys
sys.path.insert(0, r'C:\Users\soura\Documents\lightdb\bench\clickbench')
from verify_all import norm
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')
SQL = (f"SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' "
       f"WHERE SearchPhrase <> '' GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 10")
def runres(exe, env_extra=None):
    env = os.environ.copy()
    if env_extra: env.update(env_extra)
    p = subprocess.run([exe, '-c', SQL], capture_output=True, timeout=60, env=env)
    return norm(p.stdout.decode('utf-8', 'replace'))
print('Q32 determinism check (3 runs each):')
for label, env in [('inline', None), ('legacy', {'SLOTH_LEGACY_AGG':'1'}), ('duck', None)]:
    exe = DUCK if label == 'duck' else SLOTH
    seen = set()
    for i in range(3):
        seen.add(runres(exe, env))
    print(f'  {label:8s}: {len(seen)} distinct multisets across 3 runs')

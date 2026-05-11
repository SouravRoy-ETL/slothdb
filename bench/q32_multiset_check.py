"""Compare multisets for Q32 across inline/legacy/DuckDB."""
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
    return p.stdout.decode('utf-8', 'replace')
sloth_inline = runres(SLOTH)
sloth_legacy = runres(SLOTH, {'SLOTH_LEGACY_AGG': '1'})
duck = runres(DUCK)
n_inline = norm(sloth_inline); n_legacy = norm(sloth_legacy); n_duck = norm(duck)
print('inline vs legacy:', 'MATCH' if n_inline == n_legacy else 'DIFFER')
print('inline vs duck:  ', 'MATCH' if n_inline == n_duck else 'DIFFER')
print('legacy vs duck:  ', 'MATCH' if n_legacy == n_duck else 'DIFFER')

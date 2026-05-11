"""A/B Q31 with legacy ankerl+arena vs new InlineRowAgg path."""
import subprocess, time, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')
QUERIES = {
    'Q31_INT_INT': (
        f"SELECT SearchEngineID, ClientIP, COUNT(*) AS c, "
        f"SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' "
        f"WHERE SearchPhrase <> '' "
        f"GROUP BY SearchEngineID, ClientIP ORDER BY c DESC LIMIT 10"),
    'Q32_BIGINT_INT': (
        f"SELECT WatchID, ClientIP, COUNT(*) AS c, "
        f"SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' "
        f"WHERE SearchPhrase <> '' "
        f"GROUP BY WatchID, ClientIP ORDER BY c DESC LIMIT 10"),
}
def run(label, exe, sql, env_extra=None, n=3):
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    times = []
    for _ in range(n):
        t = time.perf_counter()
        subprocess.run([exe, '-c', sql], capture_output=True, timeout=60, env=env)
        times.append((time.perf_counter() - t) * 1000)
    print(f'  {label:30s} runs={[f"{x:.0f}" for x in times]} min={min(times):.0f}ms', flush=True)
    return min(times)
for name, sql in QUERIES.items():
    print(f'\n=== {name} ===')
    inline = run('SLOTH inline-row (default)', SLOTH, sql)
    legacy = run('SLOTH legacy (ankerl+arena)', SLOTH, sql, {'SLOTH_LEGACY_AGG': '1'})
    duck = run('DuckDB', DUCK, sql)
    print(f'  legacy/duck   = {legacy/duck:.2f}x slower')
    print(f'  inline/duck   = {inline/duck:.2f}x slower')
    direction = "FASTER" if inline < legacy else "SLOWER"
    print(f'  inline/legacy = {inline/legacy:.2f}x ({direction})')

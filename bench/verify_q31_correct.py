"""Verify Q31 result matches DuckDB for the SearchEngineID/ClientIP query."""
import subprocess, os
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')
SQL = f"SELECT SearchEngineID, ClientIP, COUNT(*) AS c, SUM(IsRefresh), AVG(ResolutionWidth) FROM '{DATA}' WHERE SearchPhrase <> '' GROUP BY SearchEngineID, ClientIP ORDER BY c DESC LIMIT 10"
s = subprocess.run([SLOTH, '-c', SQL], capture_output=True, timeout=120)
d = subprocess.run([DUCK, '-c', SQL], capture_output=True, timeout=120)
s_out = s.stdout.decode('utf-8', errors='replace')
d_out = d.stdout.decode('utf-8', errors='replace')
def norm(out):
    rows = []
    for line in out.splitlines():
        line = line.strip()
        if not line or line.startswith('-') or '|' not in line and '\t' not in line:
            continue
        rows.append(line)
    return rows
sr = norm(s_out); dr = norm(d_out)
print('SLOTH:');
for r in sr[:12]: print(' ', r)
print('DUCK:');
for r in dr[:12]: print(' ', r)
print('match:', sr == dr)

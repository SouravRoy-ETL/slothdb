import subprocess, time, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
exe = os.path.abspath('build/src/Release/slothdb.exe')
duck = os.path.abspath('real-life-testing/duckdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

queries = {
    'Q1':  f"SELECT COUNT(*) FROM '{data}'",
    'Q5':  f"SELECT COUNT(DISTINCT UserID) FROM '{data}'",
    'Q7':  f"SELECT AdvEngineID, COUNT(*) FROM '{data}' WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY COUNT(*) DESC",
    'Q9':  f"SELECT RegionID, COUNT(DISTINCT UserID) AS u FROM '{data}' GROUP BY RegionID ORDER BY u DESC LIMIT 10",
    'Q10': f"SELECT RegionID, SUM(AdvEngineID), COUNT(*) AS c, AVG(ResolutionWidth), COUNT(DISTINCT UserID) FROM '{data}' GROUP BY RegionID ORDER BY c DESC LIMIT 10",
    'Q11': f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10",
    'Q12': f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10",
    'Q24': f"SELECT SearchPhrase FROM '{data}' WHERE SearchPhrase <> '' ORDER BY EventTime LIMIT 10",
    'Q30': f"SELECT SUM(ResolutionWidth), SUM(ResolutionWidth + 1), SUM(ResolutionWidth + 2), SUM(ResolutionWidth + 3), SUM(ResolutionWidth + 4), SUM(ResolutionWidth + 5), SUM(ResolutionWidth + 6), SUM(ResolutionWidth + 7), SUM(ResolutionWidth + 8), SUM(ResolutionWidth + 9) FROM '{data}'",
}

# warm
def warm(bin, sql, n=2):
    for _ in range(n):
        subprocess.run([bin, '-c', sql], capture_output=True, timeout=180)

run_set = sys.argv[1:] if len(sys.argv) > 1 else list(queries.keys())

for name in run_set:
    if name not in queries: continue
    sql = queries[name]
    warm(exe, sql)
    s_t = []
    for _ in range(3):
        t0 = time.perf_counter()
        subprocess.run([exe, '-c', sql], capture_output=True, timeout=180)
        s_t.append(time.perf_counter()-t0)
    s_t.sort()
    sm = s_t[1]*1000
    print(f'{name}: sloth med={sm:.0f}ms  runs={[round(t*1000) for t in s_t]}')
    sys.stdout.flush()

import subprocess, os, sys
os.chdir(r'C:\Users\soura\Documents\lightdb')
sys.stdout.reconfigure(encoding='utf-8')
exe = os.path.abspath('build/src/Release/slothdb.exe')
duck = os.path.abspath('real-life-testing/duckdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')
q11 = f"SELECT MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhoneModel ORDER BY u DESC LIMIT 10"
q12 = f"SELECT MobilePhone, MobilePhoneModel, COUNT(DISTINCT UserID) AS u FROM '{data}' WHERE MobilePhoneModel <> '' GROUP BY MobilePhone, MobilePhoneModel ORDER BY u DESC LIMIT 10"
for name, sql in [('Q11', q11), ('Q12', q12)]:
    print(f'=== {name} sloth ===')
    print(subprocess.run([exe,'-c',sql],capture_output=True,timeout=60).stdout.decode('utf-8','replace'))
    print(f'=== {name} duck ===')
    print(subprocess.run([duck,'-c',sql],capture_output=True,timeout=60).stdout.decode('utf-8','replace'))

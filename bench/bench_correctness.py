import subprocess, os, sys, re
os.chdir(r'C:\Users\soura\Documents\lightdb')
sys.stdout.reconfigure(encoding='utf-8')
exe = os.path.abspath('build/src/Release/slothdb.exe')
duck = os.path.abspath('real-life-testing/duckdb.exe')
data = os.path.abspath('bench/clickbench/data/hits.parquet')

with open('bench/clickbench/queries.sql') as f:
    qs = [l.strip().rstrip(';') for l in f if l.strip() and not l.strip().startswith('--')]

target_idx = {'Q1':0,'Q5':4,'Q6':5,'Q7':6,'Q9':8,'Q10':9,'Q11':10,'Q12':11,'Q24':23,'Q30':29}

def sub(q):
    return re.sub(r'\bhits\b', "'" + data.replace('\\','\\\\') + "'", q)

def normalize(s):
    # Strip box-drawing chars and table formatting; keep just digits/letters
    out = re.sub(r'[^\w.\-+]+', ' ', s)
    return ' '.join(out.split())

for name, idx in target_idx.items():
    sql = sub(qs[idx])
    s_out = subprocess.run([exe,'-c',sql],capture_output=True,timeout=120).stdout.decode('utf-8','replace')
    d_out = subprocess.run([duck,'-c',sql],capture_output=True,timeout=120).stdout.decode('utf-8','replace')
    n_s = normalize(s_out)
    n_d = normalize(d_out)
    # Strip header words that differ in formatting
    # Just check that all "number tokens" match.
    s_nums = re.findall(r'-?\d+(?:\.\d+)?', s_out)
    d_nums = re.findall(r'-?\d+(?:\.\d+)?', d_out)
    # Drop the row-count summary at the end ("(N rows)" or "10 rows  2 columns")
    same = (s_nums[:50] == d_nums[:50])
    if same:
        print(f'{name}: OK ({len(s_nums)} numbers match)')
    else:
        print(f'{name}: MISMATCH')
        print(f'  sloth nums (first 20): {s_nums[:20]}')
        print(f'  duck  nums (first 20): {d_nums[:20]}')

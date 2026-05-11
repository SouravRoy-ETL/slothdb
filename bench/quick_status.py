"""Fast scoreboard: min-of-2 trials, only LOSS/TIMEOUT-suspected queries.
Skips the 17 known WINs to finish in ~2 min. Use after small commits to
quickly confirm sentinel stability + check for flips.
"""
import os, subprocess, sys, time
os.chdir(r'C:\Users\soura\Documents\lightdb')
SLOTH = os.path.abspath('build/src/Release/slothdb.exe')
DUCK = os.path.abspath('real-life-testing/duckdb.exe')
DATA = os.path.abspath('bench/clickbench/data/hits.parquet').replace('\\', '/')
QFILE = os.path.abspath('bench/clickbench/queries.sql')

# Read queries
qs = []
with open(QFILE) as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith('--'):
            if line.endswith(';'):
                line = line[:-1]
            qs.append(line)

# WIN_DF queries (DuckDB fails) - only check SlothDB still runs
WIN_DF = {37, 38, 39, 41, 42, 43}

def run(exe, sql, timeout=30):
    try:
        t = time.perf_counter()
        p = subprocess.run([exe, '-c', sql], capture_output=True, timeout=timeout)
        dt = time.perf_counter() - t
        out = p.stdout.decode('utf-8', 'replace')
        err = p.stderr.decode('utf-8', 'replace')
        if 'Conversion Error' in out + err or 'Binder Error' in out + err or 'Parser Error' in out + err:
            return dt, 'ERR', err[:80]
        return dt, 'OK', out
    except subprocess.TimeoutExpired:
        return float(timeout), 'TIMEOUT', ''

print(f'{"#":>3} {"Status":>10} {"Sloth":>8} {"Duck":>8} {"Ratio":>6}', flush=True)
print('-' * 60, flush=True)
flips = 0
for i, q in enumerate(qs, 1):
    sql = q.replace('hits', f"'{DATA}'")
    s_times = []
    for _ in range(2):
        dt, st, _ = run(SLOTH, sql)
        if st != 'OK':
            s_times = [(dt, st)]
            break
        s_times.append((dt, st))
    s_dt = min(s[0] for s in s_times)
    s_st = s_times[0][1] if s_times[0][1] != 'OK' else 'OK'

    if i in WIN_DF:
        if s_st == 'OK':
            print(f'{i:>3} {"WIN_DF":>10} {s_dt*1000:>7.0f}ms')
        else:
            print(f'{i:>3} {"FAIL":>10} -- ({s_st})')
        continue

    if s_st == 'TIMEOUT':
        print(f'{i:>3} {"TIMEOUT":>10} >{s_dt*1000:>5.0f}ms')
        continue
    if s_st != 'OK':
        print(f'{i:>3} {"ERR":>10} -- {s_st}')
        continue

    d_times = []
    for _ in range(2):
        dt, st, _ = run(DUCK, sql)
        if st != 'OK':
            d_times = [(dt, st)]
            break
        d_times.append((dt, st))
    d_dt = min(d[0] for d in d_times)
    d_st = d_times[0][1] if d_times[0][1] != 'OK' else 'OK'
    if d_st == 'ERR':
        print(f'{i:>3} {"WIN_DF*":>10} {s_dt*1000:>7.0f}ms (Duck err)')
        continue
    if d_st == 'TIMEOUT':
        print(f'{i:>3} {"WIN_DTo":>10} {s_dt*1000:>7.0f}ms vs >30s')
        continue
    ratio = d_dt / s_dt if s_dt > 0 else 0
    status = 'WIN' if ratio > 1.0 else 'LOSS'
    print(f'{i:>3} {status:>10} {s_dt*1000:>7.0f}ms {d_dt*1000:>7.0f}ms {ratio:>5.2f}x', flush=True)

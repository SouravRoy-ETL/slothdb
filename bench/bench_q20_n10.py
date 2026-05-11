"""Q20 noise check: 10 trials per engine, report distribution.

Q20 sits at 1.00x in verify_all (302ms vs 301ms). Need to see whether SlothDB
consistently wins (so raising min-of-N would flip verify_all), or whether the
two engines are genuinely tied.
"""
import os, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
SQL = f"SELECT UserID FROM '{DATA}' WHERE UserID = 435090932899640449"
N = 10

def run(exe):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.perf_counter()
    p = subprocess.run([str(exe), "-c", SQL], capture_output=True, timeout=30, env=env)
    return (time.perf_counter() - t0, p.returncode)

def measure(label, exe):
    times = []
    for i in range(N):
        dt, rc = run(exe)
        times.append(dt)
        print(f"  {label} trial {i+1}: {dt*1000:.0f}ms (rc={rc})")
    times.sort()
    print(f"  {label} sorted ms: {[f'{t*1000:.0f}' for t in times]}")
    print(f"  {label} min={times[0]*1000:.0f}ms  median={times[N//2]*1000:.0f}ms  max={times[-1]*1000:.0f}ms")
    return times

print(f"Q20 measurement: {N} trials per engine\n")
print("=== SlothDB ===")
s = measure("sloth", SLOTH)
print("\n=== DuckDB ===")
d = measure("duck", DUCK)

print(f"\n=== Verdict ===")
print(f"sloth min={s[0]*1000:.0f}ms  duck min={d[0]*1000:.0f}ms  ratio={d[0]/s[0]:.3f}x")
sloth_wins = sum(1 for i in range(N) if s[i] < d[i])
print(f"sloth-wins-by-rank: {sloth_wins}/{N}")
print(f"sloth median {s[N//2]*1000:.0f}ms vs duck median {d[N//2]*1000:.0f}ms")

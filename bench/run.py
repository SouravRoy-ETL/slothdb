"""
Reproducible benchmark runner: SlothDB vs DuckDB.

Reads a queries.sql file (one statement per line, '--' comments stripped),
substitutes a single table-name token for a quoted file path, and runs
each query through both engines via their CLI binary, capturing wall
time. Prints a per-query result line and a markdown summary table.

Usage:
    python bench/run.py --queries bench/clickbench/queries.sql \
        --table hits --data /path/to/hits.parquet \
        --slothdb build/src/Release/slothdb.exe \
        --duckdb real-life-testing/duckdb.exe \
        [--runs 3] [--warmup] [--out bench/clickbench/results.md]
"""
import argparse
import os
import re
import subprocess
import sys
import time


def parse_queries(path):
    """Return a list of SQL statements; strips '--' comments and blank lines."""
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip()
            if not line.strip() or line.lstrip().startswith("--"):
                continue
            # Allow multi-line statements? ClickBench is one-per-line.
            if line.endswith(";"):
                out.append(line[:-1])
            else:
                out.append(line)
    return out


def substitute(query, table, data):
    """Replace bare table references (e.g. FROM hits) with FROM '<data>'."""
    pattern = r"\b" + re.escape(table) + r"\b"
    return re.sub(pattern, f"'{data}'", query)


def run_one(exe, sql, timeout):
    """Time one query through `exe -c '<sql>'`. Returns (seconds, error_or_None)."""
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    exe_abs = os.path.abspath(exe)
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            [exe_abs, "-c", sql],
            capture_output=True, timeout=timeout, env=env,
        )
        elapsed = time.perf_counter() - start
        if proc.returncode != 0:
            err = proc.stderr.decode("utf-8", errors="replace").strip()
            return elapsed, err.splitlines()[0][:120] if err else "non-zero exit"
        return elapsed, None
    except subprocess.TimeoutExpired:
        return float(timeout), f"TIMEOUT (>{timeout}s)"
    except Exception as e:
        return -1.0, f"FAIL: {e}"


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return float("nan")
    if n % 2:
        return xs[n // 2]
    return (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def fmt_ms(seconds):
    if seconds < 0:
        return "FAIL"
    if seconds < 1.0:
        return f"{seconds*1000:.0f} ms"
    return f"{seconds:.2f} s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", required=True)
    ap.add_argument("--table", default="hits", help="Bare identifier in queries to replace")
    ap.add_argument("--data", required=True, help="Path or glob to data file(s)")
    ap.add_argument("--slothdb", required=True)
    ap.add_argument("--duckdb", required=True)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--warmup", action="store_true",
                    help="Run each query once before timing (defaults off)")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--out", help="Write markdown summary to this path")
    ap.add_argument("--skip", default="",
                    help="Comma-separated 1-based query indices to skip")
    args = ap.parse_args()

    skip = {int(s) for s in args.skip.split(",") if s.strip()}
    queries = parse_queries(args.queries)
    print(f"Loaded {len(queries)} queries from {args.queries}")
    print(f"Data: {args.data}")
    print(f"SlothDB: {args.slothdb}")
    print(f"DuckDB:  {args.duckdb}")
    print(f"Runs per query: {args.runs}{'  (with 1 warmup)' if args.warmup else ''}")
    print()
    print(f"{'#':>3}  {'SlothDB':>10}  {'DuckDB':>10}  {'speedup':>8}  query")
    print("-" * 100)

    rows = []
    for i, q in enumerate(queries, 1):
        if i in skip:
            print(f"{i:>3}  {'skip':>10}  {'skip':>10}  {'':>8}  {q[:60]}...")
            rows.append((i, q, -1.0, -1.0, None, None))
            continue
        sql = substitute(q, args.table, args.data)
        sloth_times = []
        duck_times = []
        sloth_err = None
        duck_err = None
        if args.warmup:
            run_one(args.slothdb, sql, args.timeout)
            run_one(args.duckdb, sql, args.timeout)
        for _ in range(args.runs):
            s, se = run_one(args.slothdb, sql, args.timeout)
            d, de = run_one(args.duckdb, sql, args.timeout)
            sloth_times.append(s)
            duck_times.append(d)
            if se and not sloth_err:
                sloth_err = se
            if de and not duck_err:
                duck_err = de
        s_med = median(sloth_times)
        d_med = median(duck_times)
        speedup = ""
        if sloth_err is None and duck_err is None and s_med > 0:
            speedup = f"{d_med/s_med:.2f}x" if d_med >= s_med else f"{d_med/s_med:.2f}x"
        s_disp = "FAIL" if sloth_err else fmt_ms(s_med)
        d_disp = "FAIL" if duck_err else fmt_ms(d_med)
        print(f"{i:>3}  {s_disp:>10}  {d_disp:>10}  {speedup:>8}  {q[:60]}{'...' if len(q) > 60 else ''}")
        rows.append((i, q, s_med, d_med, sloth_err, duck_err))

    print()
    sloth_ok = [r for r in rows if r[4] is None and r[5] is None and r[2] > 0]
    if sloth_ok:
        speedups = [r[3] / r[2] for r in sloth_ok]
        wins = sum(1 for s in speedups if s >= 1.0)
        print(f"Queries that ran on both: {len(sloth_ok)}/{len(rows)}")
        print(f"SlothDB faster on:        {wins}/{len(sloth_ok)}")
        print(f"Median speedup vs DuckDB: {median(speedups):.2f}x")
        print(f"Geomean speedup vs DuckDB: "
              f"{pow(__import__('math').prod(speedups), 1.0 / len(speedups)):.2f}x")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(f"# Benchmark results\n\n")
            f.write(f"- Queries: `{args.queries}`\n")
            f.write(f"- Data: `{args.data}`\n")
            f.write(f"- Runs per query: {args.runs}{' (with warmup)' if args.warmup else ''}\n\n")
            f.write("| # | SlothDB | DuckDB | Speedup | Query |\n")
            f.write("|--:|--:|--:|:-:|---|\n")
            for (i, q, s_med, d_med, se, de) in rows:
                if se or de:
                    s_disp = "FAIL" if se else fmt_ms(s_med)
                    d_disp = "FAIL" if de else fmt_ms(d_med)
                    sp = ""
                else:
                    s_disp = fmt_ms(s_med)
                    d_disp = fmt_ms(d_med)
                    sp = f"{d_med/s_med:.2f}x" if s_med > 0 else ""
                short = q.replace("|", "\\|")
                if len(short) > 90:
                    short = short[:87] + "..."
                f.write(f"| {i} | {s_disp} | {d_disp} | {sp} | `{short}` |\n")
            if sloth_ok:
                speedups = [r[3] / r[2] for r in sloth_ok]
                f.write("\n")
                f.write(f"**{len(sloth_ok)}/{len(rows)} queries ran on both. ")
                f.write(f"SlothDB faster on {wins}/{len(sloth_ok)}. ")
                f.write(f"Median speedup: {median(speedups):.2f}x.**\n")
        print(f"\nWrote summary to {args.out}")


if __name__ == "__main__":
    main()

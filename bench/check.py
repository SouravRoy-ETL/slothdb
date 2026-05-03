"""Run every ClickBench query through SlothDB and DuckDB, compare output for
correctness, and time each engine.

Usage:
    python bench/check.py [--queries N,M,...] [--timeout SECS]

Output: per-query line + summary at end.
"""
import argparse
import os
import re
import subprocess
import sys
import time

PARQUET = r"C:/Users/soura/Documents/lightdb/bench/clickbench/data/hits.parquet"
SLOTH = "build/src/Release/slothdb.exe"
DUCK = "real-life-testing/duckdb.exe"
QUERIES = "bench/clickbench/queries.sql"


def parse_queries(path):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip()
            if not line.strip() or line.lstrip().startswith("--"):
                continue
            out.append(line[:-1] if line.endswith(";") else line)
    return out


def substitute(query, table, data):
    pattern = r"\b" + re.escape(table) + r"\b"
    return re.sub(pattern, f"'{data}'", query)


# Numbers found in cells; ignore commas; allow integer/float/scientific.
NUM_RE = re.compile(r"[-+]?\d+\.?\d*(?:[eE][-+]?\d+)?")


SEP_RE = re.compile(r"[\-\+\s│┌┐└┘├┤┬┴┼─═╞╡╪]+")
TYPE_KEYWORDS = {"varchar", "int32", "int64", "int128", "double", "float",
                 "boolean", "date", "timestamp", "uinteger", "ubigint",
                 "smallint", "tinyint", "decimal", "interval", "time",
                 "blob", "bigint", "integer"}


def normalize(text):
    """Extract a sorted bag of data rows from a DuckDB or SlothDB CLI table.

    State-machine: a divider line (───── or -----) advances zones.
      DuckDB: top-border, names, types, mid-border, DATA, bot-border, footer.
      SlothDB: names, dashed-separator, DATA, footer.
    Once we cross into the DATA zone we collect rows until the next divider.
    """
    rows = []
    in_data = False
    seen_name_header = False
    for line in text.splitlines():
        s = line.strip()
        if not s:
            in_data = False  # blank line ends a result block
            seen_name_header = False
            continue
        if SEP_RE.fullmatch(s):
            # First divider after the name header → entering data zone.
            if seen_name_header:
                in_data = True
                seen_name_header = False
            else:
                # Either top border or end-of-data divider. Toggle off.
                in_data = False
            continue
        # Pick separator. SlothDB single-column rows have neither.
        if "│" in s:
            cells = [c.strip() for c in s.strip("│").split("│")]
        elif "|" in s:
            cells = [c.strip() for c in s.strip("|").split("|")]
        else:
            cells = [s]
        if not in_data:
            # Lines outside the data zone are name/type headers — note that
            # we saw the name header so the next divider can flip us in.
            seen_name_header = True
            continue
        # Type-name header row inside the data zone (DuckDB prints types as a
        # second header row before the inner divider).
        if any(c.lower() in TYPE_KEYWORDS for c in cells):
            continue
        # Footer: single-cell summary like "(10 rows)" or "10 rows  2 columns".
        if len(cells) == 1 and re.search(r"\b(rows?|columns?)\b", cells[0]):
            continue
        # Categorize cells: pure number → numeric (10 sig figs), else string.
        nums = []
        strs = []
        for c in cells:
            cs = c.rstrip()
            m = NUM_RE.fullmatch(cs)
            if m and cs:
                try:
                    nums.append(f"{float(cs):.10g}")
                    continue
                except ValueError:
                    pass
            strs.append(cs)
        rows.append((tuple(strs), tuple(nums)))
    return tuple(sorted(rows))


def get_free_mb():
    """Return free physical memory in MB on Windows, 0 on failure."""
    try:
        import ctypes
        class MEMSTAT(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong),
                        ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong),
                        ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong),
                        ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullExtended", ctypes.c_ulonglong)]
        m = MEMSTAT()
        m.dwLength = ctypes.sizeof(m)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ullAvailPhys // (1024 * 1024)
    except Exception:
        return 0


def wait_for_memory(min_mb=4000):
    """Block until at least min_mb MB free physical memory is available.

    The 14 GB ClickBench parquet pins ~10 GB in the OS file cache after each
    slothdb run; immediately spawning another slothdb can OOM and silently
    exit with no output (looks like SE in bench results). Polling free memory
    avoids the race without an arbitrary sleep. Threshold bumped from 2 GB to
    4 GB after observing Q13-Q16 SE'ing intermittently when total free dipped
    below the WHILE-decoding peak of slothdb (per-thread RGWork buffers can
    hit ~3 GB on multi-col group-by + distinct queries).
    """
    for _ in range(180):  # up to ~3 min
        if get_free_mb() >= min_mb:
            return
        time.sleep(1)


def run_one(exe, sql, timeout):
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    wait_for_memory()
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            [os.path.abspath(exe), "-c", sql],
            capture_output=True, timeout=timeout, env=env,
        )
        elapsed = time.perf_counter() - start
        out = proc.stdout.decode("utf-8", errors="replace")
        err = proc.stderr.decode("utf-8", errors="replace")
        if proc.returncode != 0:
            return elapsed, None, (err or out).splitlines()[0][:120] if (err or out).strip() else "non-zero exit"
        for sig in ("Conversion Error", "Binder Error", "Catalog Error",
                    "Parser Error", "IO Error", "Constraint Error",
                    "Out of Memory Error", "Internal Error", "Invalid Input Error",
                    "Not Implemented Error"):
            if sig in (out + err):
                idx = (out + err).find(sig)
                line = (out + err)[idx:].splitlines()[0][:120]
                return elapsed, None, line
        return elapsed, out, None
    except subprocess.TimeoutExpired:
        return float(timeout), None, f"TIMEOUT(>{timeout}s)"
    except Exception as e:
        return -1.0, None, f"FAIL: {e}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", default="", help="Comma-separated 1-based indices to run")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--skip", default="", help="Comma-separated 1-based indices to skip")
    args = ap.parse_args()

    sel = {int(s) for s in args.queries.split(",") if s.strip()}
    skip = {int(s) for s in args.skip.split(",") if s.strip()}
    queries = parse_queries(QUERIES)

    print(f"{'#':>3} {'sloth':>9} {'duck':>9} {'spd':>6} {'corr':>5}  query")
    print("-" * 100)

    rows = []
    for i, q in enumerate(queries, 1):
        if sel and i not in sel:
            continue
        if i in skip:
            continue
        sql = substitute(q, "hits", PARQUET)
        s_t, s_out, s_err = run_one(SLOTH, sql, args.timeout)
        d_t, d_out, d_err = run_one(DUCK, sql, args.timeout)
        # Correctness check
        corr = "?"
        if s_err and d_err:
            corr = "BB"  # both errored
        elif s_err:
            corr = "SE"  # sloth errored
        elif d_err:
            corr = "DE"  # duck errored (allowed; sloth still passes)
        else:
            sn = normalize(s_out)
            dn = normalize(d_out)
            corr = "ok" if sn == dn else "DIFF"
        spd = ""
        if s_err is None and d_err is None and s_t > 0:
            spd = f"{d_t/s_t:.2f}x"
        s_disp = "ERR" if s_err else f"{s_t*1000:.0f}ms"
        d_disp = "ERR" if d_err else f"{d_t*1000:.0f}ms"
        marker = " *" if corr == "DIFF" else ""
        print(f"{i:>3} {s_disp:>9} {d_disp:>9} {spd:>6} {corr:>5}  {q[:55]}{marker}")
        rows.append((i, q, s_t, d_t, s_err, d_err, corr))

    print()
    ok_pairs = [r for r in rows if r[4] is None and r[5] is None and r[6] == "ok"]
    diff_pairs = [r for r in rows if r[6] == "DIFF"]
    if ok_pairs:
        speedups = [r[3] / r[2] for r in ok_pairs]
        wins = sum(1 for s in speedups if s >= 1.0)
        speedups.sort()
        n = len(speedups)
        median = speedups[n // 2] if n % 2 else (speedups[n // 2 - 1] + speedups[n // 2]) / 2.0
        print(f"correct & ran-on-both: {len(ok_pairs)}/{len(rows)}")
        print(f"sloth wins (>=1.0x):   {wins}/{len(ok_pairs)}")
        print(f"median speedup:        {median:.2f}x")
    if diff_pairs:
        print(f"\nCORRECTNESS REGRESSIONS ({len(diff_pairs)}):")
        for r in diff_pairs:
            print(f"  Q{r[0]:>2}: {r[1][:80]}")


if __name__ == "__main__":
    main()

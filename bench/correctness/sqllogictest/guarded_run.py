"""Guarded sqllogictest measurement harness.

Runs each .slt file in its OWN subprocess (via runner.py --file) with a
per-file timeout. This isolates crashes (heap corruption / segfault) and
hangs to a single file: a dying subprocess is detected by the parent
(non-zero exit / timeout) and recorded as CRASH/TIMEOUT, then the run
continues. The in-process runner.py crashes the whole run (and has
destabilised the machine) when a bad file corrupts the shared heap;
subprocess isolation removes that risk.

Run:
    python guarded_run.py                 # all files, default 120s timeout
    python guarded_run.py --timeout 60    # tighter per-file timeout
    python guarded_run.py --first 10      # only the first 10 files (smoke)
    python guarded_run.py --file joins.slt  # one file
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEST_DIR = HERE / "test_files"
RUNNER = HERE / "runner.py"

# Files known to hard-crash slothdb in-process — never even spawn them.
# (Subprocess isolation would contain the crash, but skipping avoids the
# noise and the wasted process spawn.)
SKIP_FILES = {"case.slt"}

OVERALL_RE = re.compile(
    r"OVERALL: parity (\d+)/(\d+) \(([\d.]+)%\), disagree (\d+), "
    r"setup_ok_both (\d+), setup_fail_sloth_only (\d+), setup_fail_duck_only (\d+)"
)


def run_one(slt_name, timeout):
    """Run runner.py --file <slt_name> in a subprocess. Returns a dict with
    status and parsed counts (zeros on crash/timeout)."""
    res = {
        "file": slt_name, "status": "ok",
        "parity": 0, "total": 0, "disagree": 0,
        "setup_ok": 0, "setup_fail_sloth": 0, "setup_fail_duck": 0,
    }
    try:
        proc = subprocess.run(
            [sys.executable, str(RUNNER), "--file", slt_name],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        res["status"] = "TIMEOUT"
        return res
    except Exception as e:  # pragma: no cover
        res["status"] = f"SPAWN-ERR:{type(e).__name__}"
        return res

    out = proc.stdout or ""
    m = OVERALL_RE.search(out)
    if m:
        res["parity"] = int(m.group(1))
        res["total"] = int(m.group(2))
        res["disagree"] = int(m.group(4))
        res["setup_ok"] = int(m.group(5))
        res["setup_fail_sloth"] = int(m.group(6))
        res["setup_fail_duck"] = int(m.group(7))

    if proc.returncode != 0:
        # Non-zero exit: segfault / heap-corruption abort / runner error.
        # Keep any parsed counts (partial), but flag the crash.
        res["status"] = f"CRASH(rc={proc.returncode})"
    elif not m:
        res["status"] = "NO-OVERALL"
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=120,
                    help="per-file timeout in seconds (default 120)")
    ap.add_argument("--first", type=int, default=0,
                    help="only run the first N files (smoke test)")
    ap.add_argument("--file", help="run only this one .slt filename")
    args = ap.parse_args()

    files = sorted(p.name for p in TEST_DIR.glob("*.slt"))
    if args.file:
        files = [args.file]
    elif args.first:
        files = files[:args.first]

    print(f"{'file':<40} {'status':<14} {'parity/total':<14} {'disagree':<9} {'setup_ok'}")
    print("-" * 95)

    agg = {"parity": 0, "total": 0, "disagree": 0,
           "setup_ok": 0, "setup_fail_sloth": 0, "setup_fail_duck": 0}
    n_crash = n_timeout = n_skip = 0
    t_start = time.time()

    for name in files:
        if name in SKIP_FILES:
            print(f"{name:<40} {'SKIPPED':<14}")
            n_skip += 1
            continue
        r = run_one(name, args.timeout)
        print(f"{r['file']:<40} {r['status']:<14} "
              f"{str(r['parity'])+'/'+str(r['total']):<14} "
              f"{r['disagree']:<9} {r['setup_ok']}")
        for k in agg:
            agg[k] += r[k]
        if r["status"].startswith("CRASH"):
            n_crash += 1
        elif r["status"] == "TIMEOUT":
            n_timeout += 1
        sys.stdout.flush()

    elapsed = time.time() - t_start
    print("-" * 95)
    pct = (100.0 * agg["parity"] / agg["total"]) if agg["total"] else 0.0
    print(f"TOTAL parity {agg['parity']}/{agg['total']} ({pct:.1f}%)  "
          f"disagree {agg['disagree']}  setup_ok_both {agg['setup_ok']}  "
          f"setup_fail_sloth_only {agg['setup_fail_sloth']}  "
          f"setup_fail_duck_only {agg['setup_fail_duck']}")
    print(f"files: {len(files)}  crashed: {n_crash}  timed_out: {n_timeout}  "
          f"skipped: {n_skip}  wall: {elapsed:.0f}s")


if __name__ == "__main__":
    main()

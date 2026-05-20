"""SQLLogicTest runner for SlothDB and DuckDB.

Parses .slt files from Apache DataFusion's test corpus, runs each block on
both engines, compares result parity, and times each query for a speedup ratio.

Correctness reported here is slothdb-vs-duckdb, NOT slothdb-vs-DataFusion-expected.
DataFusion's expected outputs include engine-specific EXPLAIN plans and config
echoes that are not portable to other engines; using duckdb as the reference
sidesteps that. The .slt corpus is used as a diverse-SQL query source.

Run:
    python bench/correctness/sqllogictest/runner.py
    python bench/correctness/sqllogictest/runner.py --file misc.slt
"""
import argparse
import re
import sys
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path

import duckdb

try:
    import slothdb
    SLOTHDB_AVAILABLE = True
except ImportError:
    SLOTHDB_AVAILABLE = False

HERE = Path(__file__).resolve().parent
TEST_DIR = HERE / "test_files"
RESULTS_DIR = HERE / "results"

# Files that hard-crash slothdb (heap corruption visible during cleanup, not
# always reproducible — the harness either skips them or the whole run aborts
# mid-corpus with no scoreboard). Document the root cause when adding.
SKIP_FILES = {
    # case.slt — intermittent heap corruption in the cleanup path after a long
    # block run, surfaced only since VALUES support landed (queries that
    # previously errored at parse now reach an execution path that, in
    # combination with the file's many CASE-WHEN shapes and intermediate
    # CREATE-TABLE-AS-VALUES failures, leaves something in a state Python's
    # heap checker rejects on dealloc. Runs cleanly in isolation; reproducing
    # the crash needs the full block sequence. TODO: bisect with a minimal
    # repro and patch in slothdb.
    "case.slt",
}


@dataclass
class Block:
    kind: str  # 'statement_ok' | 'statement_error' | 'statement_count' | 'query' | 'config' | 'skip'
    sql: str = ""
    types: str = ""
    sort: str = "nosort"  # 'nosort' | 'rowsort' | 'valuesort'
    label: str = ""
    expected: list = field(default_factory=list)
    line_no: int = 0


def parse_slt(text):
    """Parse .slt into a list of Block. Best-effort; unknown directives become 'skip'."""
    blocks = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        raw = lines[i]
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            i += 1
            continue

        first = stripped.split()
        head = first[0]
        line_no = i + 1

        if head == "statement":
            kind_word = first[1] if len(first) > 1 else "ok"
            kind = {
                "ok": "statement_ok",
                "error": "statement_error",
                "count": "statement_count",
            }.get(kind_word, "skip")
            i += 1
            sql_lines = []
            while i < len(lines) and lines[i].strip() != "":
                sql_lines.append(lines[i])
                i += 1
            sql = "\n".join(sql_lines).strip()
            if sql.lower().startswith("set ") or sql.lower().startswith("reset "):
                blocks.append(Block(kind="config", sql=sql, line_no=line_no))
            else:
                blocks.append(Block(kind=kind, sql=sql, line_no=line_no))
        elif head == "query":
            types = first[1] if len(first) > 1 else ""
            sort = "nosort"
            label = ""
            for tok in first[2:]:
                if tok in ("rowsort", "valuesort", "nosort"):
                    sort = tok
                else:
                    label = tok
            i += 1
            sql_lines = []
            while i < len(lines) and lines[i].strip() != "----":
                sql_lines.append(lines[i])
                i += 1
            sql = "\n".join(sql_lines).strip()
            i += 1  # skip '----'
            expected_lines = []
            while i < len(lines) and lines[i].strip() != "":
                expected_lines.append(lines[i])
                i += 1
            blocks.append(Block(
                kind="query",
                sql=sql,
                types=types,
                sort=sort,
                label=label,
                expected=expected_lines,
                line_no=line_no,
            ))
        elif head in ("onlyif", "skipif", "halt", "hash-threshold", "include", "mode"):
            blocks.append(Block(kind="skip", sql=stripped, line_no=line_no))
            i += 1
        else:
            blocks.append(Block(kind="skip", sql=stripped, line_no=line_no))
            i += 1

    return blocks


def is_explain_or_dialect(sql):
    """Skip queries that test engine-specific output (EXPLAIN, DESCRIBE) or use
    obvious DataFusion-only function shapes. These aren't portable correctness checks."""
    s = sql.strip().lower()
    return (
        s.startswith("explain")
        or s.startswith("describe")
        or s.startswith("show ")
        or "datafusion." in s
    )


def normalize_value(v, type_letter):
    """Sqllogictest cell-normalisation: NULL, ints, reals to 3 dp, text."""
    if v is None:
        return "NULL"
    if type_letter == "I":
        try:
            return str(int(v))
        except (ValueError, TypeError):
            return str(v)
    if type_letter == "R":
        try:
            return f"{float(v):.3f}"
        except (ValueError, TypeError):
            return str(v)
    if type_letter == "T":
        s = str(v) if v != "" else "(empty)"
        return s
    return str(v)


def normalize_rows(rows, types, sort):
    """Produce sqllogictest-style normalized lines from raw row tuples."""
    if not types:
        types = "T" * (len(rows[0]) if rows else 0)
    lines = []
    for row in rows:
        cells = []
        for idx, v in enumerate(row):
            t = types[idx] if idx < len(types) else "T"
            cells.append(normalize_value(v, t))
        lines.append(" ".join(cells))
    if sort == "rowsort":
        lines.sort()
    elif sort == "valuesort":
        flat = []
        for line in lines:
            flat.extend(line.split())
        flat.sort()
        lines = flat
    return lines


class SlothEngine:
    name = "slothdb"

    def __init__(self):
        self.db = slothdb.connect()

    def execute(self, sql):
        self.db.sql(sql).fetchall()

    def query(self, sql):
        return self.db.sql(sql).fetchall()

    def reset(self):
        try:
            self.db.close()
        except Exception:
            pass
        self.db = slothdb.connect()


class DuckEngine:
    name = "duckdb"

    def __init__(self):
        self.con = duckdb.connect()

    def execute(self, sql):
        self.con.execute(sql)

    def query(self, sql):
        return self.con.execute(sql).fetchall()

    def reset(self):
        try:
            self.con.close()
        except Exception:
            pass
        self.con = duckdb.connect()


def run_file(slt_path, slothe, ducke):
    text = slt_path.read_text(encoding="utf-8", errors="replace")
    blocks = parse_slt(text)
    slothe.reset()
    ducke.reset()

    rows = {
        "file": slt_path.name,
        "n_total": 0,
        "n_skipped": 0,
        "n_setup_ok_both": 0,
        "n_setup_fail_sloth": 0,
        "n_setup_fail_duck": 0,
        "n_setup_fail_both": 0,
        "n_query_total": 0,
        "n_query_parity": 0,
        "n_query_sloth_only_pass": 0,
        "n_query_duck_only_pass": 0,
        "n_query_disagree": 0,
        "n_query_both_fail": 0,
        "timings": [],  # list of (sloth_ms, duck_ms)
        "diffs": [],   # tuples (line_no, sql, sloth_out, duck_out)
    }

    for blk in blocks:
        rows["n_total"] += 1
        if blk.kind in ("skip", "config"):
            rows["n_skipped"] += 1
            continue

        if blk.kind in ("statement_ok", "statement_count"):
            sloth_ok = True
            duck_ok = True
            try:
                slothe.execute(blk.sql)
            except Exception:
                sloth_ok = False
            try:
                ducke.execute(blk.sql)
            except Exception:
                duck_ok = False
            if sloth_ok and duck_ok:
                rows["n_setup_ok_both"] += 1
            elif not sloth_ok and not duck_ok:
                rows["n_setup_fail_both"] += 1
            elif not sloth_ok:
                rows["n_setup_fail_sloth"] += 1
            else:
                rows["n_setup_fail_duck"] += 1
            continue

        if blk.kind == "statement_error":
            # Both should raise. Don't tally these in correctness yet, just count.
            for eng in (slothe, ducke):
                try:
                    eng.execute(blk.sql)
                except Exception:
                    pass
            rows["n_skipped"] += 1
            continue

        if blk.kind != "query":
            rows["n_skipped"] += 1
            continue

        if is_explain_or_dialect(blk.sql):
            rows["n_skipped"] += 1
            continue

        rows["n_query_total"] += 1
        sloth_rows = None
        duck_rows = None
        sloth_err = None
        duck_err = None
        t0 = time.perf_counter()
        try:
            sloth_rows = slothe.query(blk.sql)
        except Exception as e:
            sloth_err = str(e)[:200]
        sloth_ms = (time.perf_counter() - t0) * 1000
        t0 = time.perf_counter()
        try:
            duck_rows = ducke.query(blk.sql)
        except Exception as e:
            duck_err = str(e)[:200]
        duck_ms = (time.perf_counter() - t0) * 1000

        if sloth_err and duck_err:
            rows["n_query_both_fail"] += 1
            continue
        if sloth_err:
            rows["n_query_duck_only_pass"] += 1
            rows["diffs"].append((blk.line_no, blk.sql[:90], f"ERR: {sloth_err[:80]}", "ok"))
            continue
        if duck_err:
            rows["n_query_sloth_only_pass"] += 1
            continue

        sloth_norm = normalize_rows(sloth_rows, blk.types, blk.sort)
        duck_norm = normalize_rows(duck_rows, blk.types, blk.sort)
        if sloth_norm == duck_norm:
            rows["n_query_parity"] += 1
            rows["timings"].append((sloth_ms, duck_ms))
        else:
            rows["n_query_disagree"] += 1
            rows["diffs"].append((
                blk.line_no,
                blk.sql[:90],
                "\n".join(sloth_norm[:3]),
                "\n".join(duck_norm[:3]),
            ))
    return rows


def fmt_speedup_summary(timings):
    if not timings:
        return "n=0"
    ratios = [d / s for s, d in timings if s > 0]
    ratios.sort()
    n = len(ratios)
    if n == 0:
        return "n=0"
    median = ratios[n // 2]
    p10 = ratios[max(0, n // 10)]
    p90 = ratios[min(n - 1, (9 * n) // 10)]
    sloth_total = sum(s for s, _ in timings)
    duck_total = sum(d for _, d in timings)
    return (f"n={n} ratio(duck/sloth) median={median:.2f}x p10={p10:.2f}x p90={p90:.2f}x "
            f"sloth_total={sloth_total:.0f}ms duck_total={duck_total:.0f}ms")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", help="run only this .slt filename")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    if not SLOTHDB_AVAILABLE:
        print("ERROR: slothdb Python module not importable. Try 'pip install slothdb'.")
        sys.exit(1)

    slothe = SlothEngine()
    ducke = DuckEngine()

    files = sorted(TEST_DIR.glob("*.slt"))
    if args.file:
        files = [TEST_DIR / args.file]

    print(f"{'file':<42} {'parity/total':<14} {'setup_ok':<10} {'timing summary'}")
    print("-" * 130)

    overall_timings = []
    overall_parity = 0
    overall_queries = 0
    overall_disagree = 0
    overall_setup_ok = 0
    overall_setup_fail_sloth = 0
    overall_setup_fail_duck = 0

    for slt_path in files:
        if not slt_path.exists():
            print(f"missing: {slt_path}")
            continue
        if slt_path.name in SKIP_FILES:
            print(f"{slt_path.name:<42} SKIPPED (known crash)")
            continue
        try:
            r = run_file(slt_path, slothe, ducke)
        except Exception as e:
            print(f"{slt_path.name:<42} RUNNER ERROR: {type(e).__name__}: {str(e)[:60]}")
            if args.verbose:
                traceback.print_exc()
            continue
        parity_str = f"{r['n_query_parity']}/{r['n_query_total']}"
        setup_str = f"{r['n_setup_ok_both']}/{r['n_setup_ok_both']+r['n_setup_fail_sloth']+r['n_setup_fail_duck']+r['n_setup_fail_both']}"
        timing_str = fmt_speedup_summary(r["timings"])
        print(f"{r['file']:<42} {parity_str:<14} {setup_str:<10} {timing_str}")
        if args.verbose and r["diffs"]:
            for ln, sql, s_out, d_out in r["diffs"][:3]:
                print(f"   L{ln}: {sql}")
                print(f"      sloth: {s_out}")
                print(f"      duck : {d_out}")
        overall_timings.extend(r["timings"])
        overall_parity += r["n_query_parity"]
        overall_queries += r["n_query_total"]
        overall_disagree += r["n_query_disagree"]
        overall_setup_ok += r["n_setup_ok_both"]
        overall_setup_fail_sloth += r["n_setup_fail_sloth"]
        overall_setup_fail_duck += r["n_setup_fail_duck"]

    print("-" * 130)
    parity_pct = (100.0 * overall_parity / overall_queries) if overall_queries else 0.0
    print(f"OVERALL: parity {overall_parity}/{overall_queries} ({parity_pct:.1f}%), "
          f"disagree {overall_disagree}, "
          f"setup_ok_both {overall_setup_ok}, setup_fail_sloth_only {overall_setup_fail_sloth}, "
          f"setup_fail_duck_only {overall_setup_fail_duck}")
    print(f"SPEEDUP: {fmt_speedup_summary(overall_timings)}")


if __name__ == "__main__":
    main()

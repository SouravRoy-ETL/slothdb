"""Dump FULL disagreement + sloth-only-error detail for one .slt file.

runner.py's -v caps diffs at 3 and truncates SQL/outputs. This dumps every
query where slothdb and duckdb both run but disagree (DISAGREE), plus every
query where slothdb errors but duckdb runs (SLOTH_ERR) — the two actionable
correctness pools — with full SQL and full normalized outputs.

Run per-file (subprocess-isolated by the caller):
    python dump_diffs.py --file expr.slt
"""
import argparse
import sys
from pathlib import Path

# Force UTF-8 stdout so emoji/non-Latin SQL doesn't crash the dumper on
# Windows (default cp1252 console codec).
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

from runner import (parse_slt, normalize_rows, is_explain_or_dialect,
                    SlothEngine, DuckEngine, TEST_DIR)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True)
    args = ap.parse_args()

    path = TEST_DIR / args.file
    text = path.read_text(encoding="utf-8", errors="replace")
    blocks = parse_slt(text)

    slothe = SlothEngine()
    ducke = DuckEngine()
    slothe.reset()
    ducke.reset()

    n_disagree = 0
    n_sloth_err = 0
    for blk in blocks:
        if blk.kind in ("skip", "config", "statement_error"):
            continue
        if blk.kind in ("statement_ok", "statement_count"):
            # Run setup on both so later queries have their tables.
            for eng in (slothe, ducke):
                try:
                    eng.execute(blk.sql)
                except Exception:
                    pass
            continue
        if blk.kind != "query":
            continue
        if is_explain_or_dialect(blk.sql):
            continue

        sloth_rows = duck_rows = None
        sloth_err = duck_err = None
        try:
            sloth_rows = slothe.query(blk.sql)
        except Exception as e:
            sloth_err = str(e)
        try:
            duck_rows = ducke.query(blk.sql)
        except Exception as e:
            duck_err = str(e)

        if duck_err:
            # duck can't run it either — not an actionable parity target.
            continue
        if sloth_err:
            n_sloth_err += 1
            print(f"=== SLOTH_ERR L{blk.line_no} ===")
            print(f"SQL: {blk.sql}")
            print(f"ERR: {sloth_err[:400]}")
            print()
            continue

        sloth_norm = normalize_rows(sloth_rows, blk.types, blk.sort)
        duck_norm = normalize_rows(duck_rows, blk.types, blk.sort)
        if sloth_norm != duck_norm:
            n_disagree += 1
            print(f"=== DISAGREE L{blk.line_no} ===")
            print(f"SQL: {blk.sql}")
            print(f"SLOTH ({len(sloth_norm)} rows): {sloth_norm[:25]}")
            print(f"DUCK  ({len(duck_norm)} rows): {duck_norm[:25]}")
            print()

    print(f"### SUMMARY {args.file}: disagree={n_disagree} sloth_err={n_sloth_err}")


if __name__ == "__main__":
    main()

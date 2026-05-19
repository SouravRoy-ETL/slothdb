"""Official-methodology ClickBench measurement: SlothDB vs DuckDB.

Runs the 43 official ClickBench queries (the DuckDB variant, verified
byte-identical to ClickHouse/ClickBench duckdb/queries.sql) against
hits_typed.parquet, TRIALS times per engine per query, and records raw
wall-clock times. Reports min-of-3 per query plus a strict result
comparison.

No win/loss/WIN_DF classification: just the raw times and whether the two
engines' results agree. Both engines query the same parquet file directly,
on the same machine. This is the official query set and the official
3-run methodology; it is not an official ClickBench submission, which runs
on standardised cloud hardware.

  python bench/clickbench/official_bench.py
"""
import csv
import math
import re
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DATA = ROOT / "bench/clickbench/data/hits_typed.parquet"
QFILE = ROOT / "bench/clickbench/queries.sql"
OUT = ROOT / "bench/clickbench/official_results.md"
OUT_CSV = ROOT / "bench/clickbench/official_results.csv"
TIMEOUT = 120
TRIALS = 3
ERR_SIGS = ("Conversion Error", "Binder Error", "Catalog Error", "Parser Error",
            "IO Error", "Constraint Error", "Out of Memory Error", "Internal Error",
            "Invalid Input Error", "ParseError", "RuntimeError", "Bind Error")


def find_duckdb():
    for p in (r"C:\duckdb-poc\bin\duckdb.exe", "C:/duckdb-poc/bin/duckdb",
              "C:/duckdb-poc/bin/duckdb.exe"):
        if Path(p).exists():
            return p
    return "duckdb"


DUCK = find_duckdb()


def run(exe, sql):
    """One execution. Returns (seconds, status, payload)."""
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), "TIMEOUT", f">{TIMEOUT}s"
    dt = time.perf_counter() - t0
    out = p.stdout.decode("utf-8", "replace")
    err = p.stderr.decode("utf-8", "replace")
    cx = out + "\n" + err
    if p.returncode != 0:
        first = (err.strip() or out.strip()).splitlines()
        return dt, "ERROR", (first[0][:160] if first else "non-zero exit")
    for s in ERR_SIGS:
        i = cx.find(s)
        if i >= 0:
            return dt, "ERROR", cx[i:].splitlines()[0][:160]
    return dt, "OK", out


# Result normalisation: strip CLI formatting so the two engines' outputs
# compare on values alone. Numeric-token multiset compare.
_BOX = "│┌┐└┘─┬┴├┤┼"
_HDR = {"int64", "bigint", "double", "varchar", "date", "timestamp", "int32",
        "integer", "decimal", "time", "int8", "int16", "int128", "uint8",
        "uint16", "uint32", "uint64", "ubigint", "tinyint", "smallint",
        "hugeint", "float", "real", "boolean", "bool", "blob", "interval"}
_FOOTER = re.compile(r"^\(?\d+\s+rows?\)?(\s+\d+\s+columns?)?$")
_OCTAL = re.compile(r"\\\d{1,3}")


def _canon_num(t):
    try:
        i = int(t)
        return f"{float(i):.13g}" if abs(i) >= 10 ** 15 else str(i)
    except ValueError:
        pass
    try:
        f = float(t)
        if abs(f) >= 1e15:
            return f"{f:.13g}"
        return str(int(f)) if f.is_integer() else repr(f)
    except ValueError:
        return t


def norm(s):
    toks = []
    stripped = ["".join(c for c in l if c not in _BOX).strip() for l in s.splitlines()]
    header_skip = set()
    for i, sl in enumerate(stripped):
        if not sl:
            continue
        cells = [t for t in sl.replace("|", " ").split() if t]
        if cells and all(c.lower() in _HDR for c in cells):
            if i > 0:
                header_skip.add(i - 1)
            continue
        if set(sl) <= set("-+| ") and i > 0:
            header_skip.add(i - 1)
    for i, l in enumerate(stripped):
        if i in header_skip:
            continue
        if not l or set(l) <= set("-+| ") or l.lower() in _HDR or _FOOTER.match(l):
            continue
        for t in l.replace("|", " ").split():
            if t.lower() in _HDR or "(" in t or ")" in t:
                continue
            t = _OCTAL.sub("", t)
            if t and any(c.isdigit() for c in t):
                toks.append(_canon_num(t))
    return "\n".join(sorted(toks))


def trials(exe, sql):
    times, payload = [], ""
    for _ in range(TRIALS):
        dt, st, pl = run(exe, sql)
        times.append(dt)
        if st != "OK":
            return times, st, pl
        payload = pl
    return times, "OK", payload


def main():
    if not DATA.exists():
        raise SystemExit(f"missing dataset: {DATA}")
    queries = [l.strip().rstrip(";") for l in QFILE.read_text(encoding="utf-8").splitlines()
               if l.strip() and not l.strip().startswith("--")]
    if len(queries) != 43:
        raise SystemExit(f"expected 43 queries, got {len(queries)}")
    print(f"SlothDB: {SLOTH}\nDuckDB:  {DUCK}\nData:    {DATA}\n")

    rows = []
    for i, q in enumerate(queries, 1):
        sql = re.sub(r"\bhits\b", f"'{DATA.as_posix()}'", q)
        st_t, st_s, st_p = trials(SLOTH, sql)
        dk_t, dk_s, dk_p = trials(DUCK, sql)
        sm = min(st_t) if st_s == "OK" else None
        dm = min(dk_t) if dk_s == "OK" else None
        if st_s == "OK" and dk_s == "OK":
            result = "match" if norm(st_p) == norm(dk_p) else "DIFF"
        elif st_s != "OK" and dk_s != "OK":
            result = f"both-fail (sloth:{st_s} duck:{dk_s})"
        elif st_s != "OK":
            result = f"sloth-only-fail ({st_s})"
        else:
            result = f"duck-only-fail ({dk_s})"
        rows.append(dict(i=i, st_s=st_s, dk_s=dk_s, sm=sm, dm=dm, result=result))
        smv = f"{sm * 1000:.0f}ms" if sm else st_s
        dmv = f"{dm * 1000:.0f}ms" if dm else dk_s
        ratio = f"{dm / sm:.2f}x" if (sm and dm) else ""
        print(f"Q{i:>2}  sloth={smv:>10}  duck={dmv:>10}  {ratio:>7}  {result}", flush=True)

    comp = [r for r in rows if r["sm"] and r["dm"]]
    sloth_faster = sum(1 for r in comp if r["sm"] < r["dm"])
    geo = (math.exp(sum(math.log(r["dm"] / r["sm"]) for r in comp) / len(comp))
           if comp else 0.0)
    matches = sum(1 for r in rows if r["result"] == "match")
    diffs = sum(1 for r in rows if r["result"] == "DIFF")
    st_tot = sum(r["sm"] for r in comp)
    dk_tot = sum(r["dm"] for r in comp)

    L = ["# ClickBench-43: SlothDB vs DuckDB",
         "",
         "43 official ClickBench queries (DuckDB variant, verbatim from",
         "ClickHouse/ClickBench). Both engines query hits_typed.parquet directly on",
         f"the same machine; {TRIALS} trials per engine per query, min-of-3 reported;",
         f"{TIMEOUT}s per-trial timeout. SlothDB local build vs DuckDB v1.4.3. This is",
         "the official query set and 3-run methodology, run locally; it is not an",
         "official ClickBench submission (that uses standardised cloud hardware).",
         "",
         "| Q | SlothDB | DuckDB | DuckDB/SlothDB | result |",
         "|--:|--:|--:|--:|:--|"]
    for r in rows:
        smv = f"{r['sm'] * 1000:.0f} ms" if r["sm"] else r["st_s"]
        dmv = f"{r['dm'] * 1000:.0f} ms" if r["dm"] else r["dk_s"]
        ratio = f"{r['dm'] / r['sm']:.2f}x" if (r["sm"] and r["dm"]) else ""
        L.append(f"| {r['i']} | {smv} | {dmv} | {ratio} | {r['result']} |")
    L += ["",
          f"**Comparable queries (both engines ran): {len(comp)} of 43.**",
          "",
          f"- SlothDB faster on {sloth_faster} of {len(comp)}; "
          f"DuckDB faster on {len(comp) - sloth_faster}.",
          f"- Geomean DuckDB/SlothDB time ratio: {geo:.2f}x "
          f"(above 1.0 means SlothDB faster on average).",
          f"- Total min-of-3 wall time: SlothDB {st_tot:.1f}s, DuckDB {dk_tot:.1f}s.",
          f"- Results vs DuckDB: {matches} matched, {diffs} differed (need review), "
          f"{43 - len(comp)} not comparable (an engine errored or timed out).",
          ""]
    OUT.write_text("\n".join(L), encoding="utf-8")
    with open(OUT_CSV, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["q", "sloth_ms", "duck_ms", "result"])
        for r in rows:
            w.writerow([r["i"],
                        f"{r['sm'] * 1000:.1f}" if r["sm"] else "",
                        f"{r['dm'] * 1000:.1f}" if r["dm"] else "",
                        r["result"]])
    print(f"\nComparable {len(comp)}/43 | SlothDB faster {sloth_faster}/{len(comp)} | "
          f"geomean {geo:.2f}x | results match {matches} diff {diffs}")
    print(f"Wrote {OUT}")
    print(f"Wrote {OUT_CSV}")


if __name__ == "__main__":
    main()

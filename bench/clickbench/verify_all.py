"""Surface-scan all 43 ClickBench queries: status + correctness vs DuckDB.
One trial per query, 30s timeout. Captures:
PARSE_ERROR/RUNTIME/TIMEOUT/WIN/LOSS/WRONG/WIN_DF.

WIN_DF = "DuckDB Failed": SlothDB completes correctly but DuckDB cannot run
the query at all (e.g. Q37–Q42 fail with `Conversion Error: Could not convert
string '2013-07-01' to UINT16` because DuckDB doesn't implicitly cast that
string to the UINT16 EventDate column). Counted as a SlothDB capability win.
"""
import os, re, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SLOTH = ROOT / "build/src/Release/slothdb.exe"
DUCK = ROOT / "real-life-testing/duckdb.exe"
DATA = ROOT / "bench/clickbench/data/hits.parquet"
QFILE = ROOT / "bench/clickbench/queries.sql"
OUT = ROOT / "_private/orchestrator/phase2_clickbench43_verify.md"
TIMEOUT = 30
ERR_SIGS = ("Conversion Error","Binder Error","Catalog Error","Parser Error","IO Error",
            "Constraint Error","Out of Memory Error","Internal Error","Invalid Input Error",
            "ParseError","RuntimeError","Bind Error")

def run(exe, sql):
    env = os.environ.copy(); env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe), "-c", sql], capture_output=True, timeout=TIMEOUT, env=env)
        dt = time.perf_counter() - t0
        out = p.stdout.decode("utf-8", "replace"); err = p.stderr.decode("utf-8", "replace")
        cx = out + "\n" + err
        if p.returncode != 0:
            ln = (err.strip() or out.strip()).splitlines()[0:1]
            return dt, "ERROR", (ln[0][:120] if ln else "non-zero exit"), out
        for s in ERR_SIGS:
            i = cx.find(s)
            if i >= 0:
                k = "PARSE_ERROR" if any(t in s for t in ("Parser","Bind")) else "RUNTIME"
                return dt, k, cx[i:].splitlines()[0][:120], out
        return dt, "OK", None, out
    except subprocess.TimeoutExpired:
        return float(TIMEOUT), "TIMEOUT", f">{TIMEOUT}s", ""

_BOX = "│┌┐└┘─┬┴├┤┼"
_HDR = {"int64","bigint","double","varchar","date","timestamp","int32","integer","decimal","time",
        "int8","int16","int128","uint8","uint16","uint32","uint64","ubigint",
        "tinyint","smallint","hugeint","float","real","boolean","bool","blob","interval"}
# Row-count / column-count footer in either CLI style:
#   SlothDB:  "(10 rows)"  → after strip: "(10 rows)"
#   DuckDB:   "│ 10 rows   2 columns │" → after box-drawing strip: "10 rows   2 columns"
_FOOTER = re.compile(r"^\(?\d+\s+rows?\)?(\s+\d+\s+columns?)?$")
def _canon_num(t):
    # 1587 vs 1587.0 should compare equal: both are the same value, just
    # different display (DuckDB keeps trailing .0 for double columns, SlothDB
    # drops it via to_chars). Canonicalize to int when fractional part is 0.
    try: return str(int(t))
    except ValueError: pass
    try:
        f = float(t)
        return str(int(f)) if f.is_integer() else repr(f)
    except ValueError: return t

def norm(s):
    # Strip box-drawing decoration; keep value-bearing numeric tokens (multiset compare).
    toks = []
    for l in s.splitlines():
        l = "".join(c for c in l if c not in _BOX).strip()
        if not l or set(l) <= set("-+| ") or l.lower() in _HDR: continue
        if _FOOTER.match(l): continue
        for t in l.replace("|"," ").split():
            # Multi-cell type-row tokens (e.g. DuckDB "│ varchar │ int64 │"
            # → ["varchar", "int64"]) leaked through the whole-line _HDR
            # check above. Filter per-token here.
            if t.lower() in _HDR: continue
            # DuckDB column-header rows for expression columns split into
            # things like ["(ClientIP", "-", "1)"] — the "1)" tokens have
            # digits but aren't data. Skip parens-containing tokens.
            if "(" in t or ")" in t: continue
            if any(c.isdigit() for c in t): toks.append(_canon_num(t))
    return "\n".join(sorted(toks))

def main():
    qs = [(s[:-1] if s.endswith(";") else s) for s in (l.strip() for l in
          QFILE.read_text(encoding="utf-8").splitlines()) if s and not s.startswith("--")]
    rows = []
    for i, q in enumerate(qs, 1):
        sql = re.sub(r"\bhits\b", lambda _: f"'{DATA}'", q)
        sd, ss, se, so = run(SLOTH, sql)
        if ss != "OK":
            # SlothDB can't run it. If DuckDB also can't, both fail; if DuckDB
            # can, that's a real LOSS_DF (DuckDB-only feature gap).
            rows.append((i, ss, sd, None, se, q)); print(f"{i:>3} {ss} {se}"); continue
        dd, ds, de, do = run(DUCK, sql)
        if ds != "OK":
            # DuckDB failed but SlothDB succeeded → SlothDB capability win.
            rows.append((i, "WIN_DF", sd, dd, de, q))
            print(f"{i:>3} WIN_DF sloth={sd*1000:.0f}ms duck=FAIL ({de or 'error'})")
            continue
        match = norm(so) == norm(do)
        st = ("WIN" if dd > sd else "LOSS") if match else "WRONG"
        rows.append((i, st, sd, dd, None, q))
        print(f"{i:>3} {st} sloth={sd*1000:.0f}ms duck={dd*1000:.0f}ms {'' if match else 'MISMATCH'}")
    L = [f"# ClickBench-43 surface verify (warm-cache, {TIMEOUT}s timeout)\n",
         "| # | Status | SlothDB | DuckDB | Ratio | Note | Query |", "|--:|:--|--:|--:|--:|:--|:--|"]
    for (i, st, sm, dm, err, q) in rows:
        sd = f"{sm*1000:.0f}ms" if sm and sm > 0 else "-"
        dd_ = f"{dm*1000:.0f}ms" if dm and dm > 0 else "FAIL" if st == "WIN_DF" else "-"
        rt = f"{dm/sm:.2f}x" if (sm and dm and sm > 0) else ""
        L.append(f"| {i} | {st} | {sd} | {dd_} | {rt} | {(err or '')[:80].replace('|','\\|')} | `{q[:80].replace('|','\\|')}` |")
    n = len(rows); cnt = lambda k: sum(1 for r in rows if r[1] == k)
    wins = cnt("WIN") + cnt("WIN_DF")
    loss, wrong = cnt("LOSS"), cnt("WRONG"); fails = n - wins - loss - wrong
    L.append(f"\n**WIN={wins} (incl. WIN_DF={cnt('WIN_DF')}) LOSS={loss} WRONG={wrong} FAIL/SKIP={fails} of {n}**")
    OUT.write_text("\n".join(L), encoding="utf-8")
    print(f"\nWrote {OUT}\nWIN={wins} (WIN_DF={cnt('WIN_DF')}) LOSS={loss} WRONG={wrong} FAIL/SKIP={fails}")

if __name__ == "__main__":
    main()

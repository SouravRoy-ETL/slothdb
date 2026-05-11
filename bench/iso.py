"""Single-query isolated bench: 1 trial, 30s timeout, both engines."""
import argparse, os, subprocess, time, sys

def run(exe, sql, timeout=30):
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    s = time.perf_counter()
    try:
        p = subprocess.run([os.path.abspath(exe), "-c", sql],
                           capture_output=True, timeout=timeout, env=env)
        e = time.perf_counter() - s
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        if p.returncode != 0:
            return e, "FAIL: " + (err.strip() or out.strip()).splitlines()[0][:80]
        for sig in ("Conversion Error","Binder Error","Catalog Error","Parser Error",
                    "IO Error","Constraint Error","Out of Memory","Internal Error"):
            if sig in (out+err):
                return e, "FAIL: " + sig
        return e, out.strip().splitlines()[-3:] if out.strip() else None
    except subprocess.TimeoutExpired:
        return float(timeout), f"TIMEOUT >{timeout}s"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qnum", type=int, required=True, help="ClickBench query #")
    ap.add_argument("--data", default="bench/clickbench/data/hits.parquet")
    ap.add_argument("--sloth", default="build/src/Release/slothdb.exe")
    ap.add_argument("--duck", default="real-life-testing/duckdb.exe")
    ap.add_argument("--queries", default="bench/clickbench/queries.sql")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--show-output", action="store_true")
    a = ap.parse_args()

    qs=[]
    with open(a.queries) as f:
        for line in f:
            line=line.rstrip()
            if line.strip() and not line.lstrip().startswith("--"):
                qs.append(line[:-1] if line.endswith(";") else line)
    sql = qs[a.qnum-1].replace(" hits", f" '{a.data}'", 1).replace(" hits;", f" '{a.data}';")
    if "FROM hits" in qs[a.qnum-1]:
        sql = qs[a.qnum-1].replace("FROM hits", f"FROM '{a.data}'")
    print(f"Q{a.qnum}: {sql[:120]}{'...' if len(sql)>120 else ''}")
    print(f"timeout={a.timeout}s runs={a.runs}\n")
    for trial in range(a.runs):
        st, sd = run(a.sloth, sql, a.timeout)
        dt, dd = run(a.duck, sql, a.timeout)
        sp = (dt/st) if (sd is None or isinstance(sd, list)) and (dd is None or isinstance(dd, list)) and st>0 else 0
        print(f"  trial {trial+1}: sloth={st*1000:.0f}ms duck={dt*1000:.0f}ms speedup={sp:.2f}x"
              + ("  [sloth: " + (sd if isinstance(sd, str) else "OK") + "]" if isinstance(sd, str) else "")
              + ("  [duck: " + (dd if isinstance(dd, str) else "OK") + "]" if isinstance(dd, str) else ""))
        if a.show_output and isinstance(sd, list) and isinstance(dd, list):
            print("    sloth:", sd[-3:])
            print("    duck :", dd[-3:])

if __name__ == "__main__":
    main()

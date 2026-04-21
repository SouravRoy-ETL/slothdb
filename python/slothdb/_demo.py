"""Self-contained demo for `python -c "import slothdb; slothdb.demo()"`.

Generates a 100k-row synthetic sales CSV, runs 3 queries through SlothDB with
timing, and if `duckdb` is importable, runs the same queries through DuckDB so
the user sees a real head-to-head number on their own machine.
"""

import csv
import os
import random
import time
from pathlib import Path

from . import connect

ROWS = 100_000
REGIONS = ["north", "south", "east", "west", "central"]
PRODUCTS = ["widget", "gadget", "gizmo", "doohickey", "thingamajig"]


def _sample_path():
    home = Path(os.path.expanduser("~")) / ".slothdb" / "demo"
    home.mkdir(parents=True, exist_ok=True)
    return home / f"sales_{ROWS}.csv"


def _generate_if_missing(path):
    if path.exists() and path.stat().st_size > 0:
        return
    print(f"Generating sample data at {path} ({ROWS:,} rows, ~3 MB)...")
    rng = random.Random(42)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "region", "product", "year", "revenue", "qty"])
        for i in range(ROWS):
            w.writerow([
                i,
                rng.choice(REGIONS),
                rng.choice(PRODUCTS),
                rng.randint(2019, 2024),
                round(rng.uniform(10, 10000), 2),
                rng.randint(1, 100),
            ])


def _time_query(runner, sql_text):
    t0 = time.perf_counter()
    result = runner(sql_text)
    ms = (time.perf_counter() - t0) * 1000.0
    return ms, result


def run():
    path = _sample_path()
    _generate_if_missing(path)

    queries = [
        ("COUNT(*)",
         f"SELECT COUNT(*) FROM '{path}'"),
        ("SUM(revenue) WHERE year>=2023",
         f"SELECT SUM(revenue) FROM '{path}' WHERE year >= 2023"),
        ("GROUP BY region",
         f"SELECT region, SUM(revenue) FROM '{path}' GROUP BY region ORDER BY 2 DESC"),
    ]

    db = connect()
    slothdb_runner = lambda q: db.sql(q)

    duckdb_runner = None
    try:
        import duckdb  # type: ignore
        ddb = duckdb.connect()
        duckdb_runner = lambda q: ddb.execute(q).fetchall()
    except ImportError:
        pass

    print()
    print("=" * 60)
    print(f"SlothDB demo — {ROWS:,} rows, CSV on disk")
    print("=" * 60)
    print()

    header = f"{'Query':<32} {'SlothDB':>10}"
    if duckdb_runner:
        header += f" {'DuckDB':>10} {'Speedup':>10}"
    print(header)
    print("-" * len(header))

    for label, q in queries:
        s_ms, s_res = _time_query(slothdb_runner, q)
        line = f"{label:<32} {s_ms:>8.1f} ms"
        if duckdb_runner:
            d_ms, _ = _time_query(duckdb_runner, q)
            speedup = d_ms / s_ms if s_ms > 0 else 0.0
            line += f" {d_ms:>8.1f} ms {speedup:>8.2f}x"
        print(line)

    print()
    print("Sample result (GROUP BY region):")
    db.sql(queries[-1][1]).show()

    print()
    print("=" * 60)
    if duckdb_runner is None:
        print("Tip: pip install duckdb — rerun to see side-by-side timing.")
    print("Liked it?   Star the repo: https://github.com/SouravRoy-ETL/slothdb")
    print("Broke it?   File an issue: https://github.com/SouravRoy-ETL/slothdb/issues")
    print("=" * 60)

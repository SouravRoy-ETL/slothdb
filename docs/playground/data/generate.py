"""Generate the deterministic sales demo in both CSV and Parquet forms.

Run once; the outputs (sales.csv, sales.parquet) are committed. Boot fetches
them and writes to MEMFS at /data/sales.*.
"""
import os
import pyarrow as pa
import pyarrow.parquet as pq

HERE = os.path.dirname(os.path.abspath(__file__))

regions  = ["NA", "EU", "APAC", "LATAM"]
products = ["Widget", "Gadget", "Sprocket", "Cog"]
years    = [2022, 2023, 2024]

# Numerical-recipes LCG — identical to the JS generator so data matches.
def lcg():
    seed = 1
    while True:
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        yield seed / 0x100000000

g = lcg()
def rnd(): return next(g)

N = 1000
rows = []
for _ in range(N):
    region  = regions[int(rnd() * len(regions))]
    product = products[int(rnd() * len(products))]
    year    = years[int(rnd() * len(years))]
    qty     = 1 + int(rnd() * 200)
    rev     = (10 + int(rnd() * 990)) * qty
    rows.append((region, product, year, qty, rev))

# CSV — plain, no quoting needed (no commas in any field).
with open(os.path.join(HERE, "sales.csv"), "w", newline="\n") as f:
    f.write("region,product,year,qty,revenue\n")
    for r in rows:
        f.write(",".join(str(x) for x in r) + "\n")

# Parquet — typed columns, SNAPPY compression.
table = pa.table({
    "region":  pa.array([r[0] for r in rows], type=pa.string()),
    "product": pa.array([r[1] for r in rows], type=pa.string()),
    "year":    pa.array([r[2] for r in rows], type=pa.int32()),
    "qty":     pa.array([r[3] for r in rows], type=pa.int32()),
    "revenue": pa.array([r[4] for r in rows], type=pa.int64()),
})
pq.write_table(table, os.path.join(HERE, "sales.parquet"), compression="snappy")

csv_bytes = os.path.getsize(os.path.join(HERE, "sales.csv"))
pq_bytes  = os.path.getsize(os.path.join(HERE, "sales.parquet"))
print(f"sales.csv:     {N} rows, {csv_bytes:,} bytes")
print(f"sales.parquet: {N} rows, {pq_bytes:,} bytes")

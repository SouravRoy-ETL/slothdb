# bench/correctness/

External correctness benchmarks for SlothDB, each compared head-to-head against DuckDB.

Three benchmarks, sequenced:

1. **`sqllogictest/`** — Apache DataFusion's [sqllogictest](https://github.com/apache/datafusion/tree/main/datafusion/sqllogictest) corpus. ~158 `.slt` files covering aggregates, joins, window functions, type coercion, casts, expressions. Used as a query source; correctness measured as parity with DuckDB (not against DataFusion-specific expected output, which embeds engine-specific EXPLAIN plans and config).

2. **`taxi/`** — [pdet/taxi-benchmark](https://github.com/pdet/taxi-benchmark). NYC taxi dataset, geo + time aggregates. Planned.

3. **`pollock/`** — [HPI-Information-Systems/Pollock](https://github.com/HPI-Information-Systems/Pollock). CSV parsing correctness on malformed and edge-case inputs. Planned.

Each subdirectory has its own README with methodology and current status. Speedup is reported as `duckdb_time / slothdb_time` on the subset where both engines agree on the result; raw timings are recorded too so the noise floor is visible.

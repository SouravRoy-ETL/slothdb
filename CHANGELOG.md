# Changelog

All notable changes to SlothDB are documented here.

## 0.1.3 — Arrow + SQLite on the fast path

Arrow IPC and SQLite were the last two readers still using the bulk-load-to-DataTable roundtrip. Both now stream typed `DataChunk`s at execution time — the same pattern Parquet / JSON / Avro / CSV use.

- **`PhysicalArrowScan`** — `ArrowIPCReader::DetectSchemaLight()` + `ReadIntoChunks()` stream rows directly into `int32_t[]` / `int64_t[]` / `double[]` / `string_t[]` arrays. No Value-boxed intermediate.
- **`PhysicalSQLiteScan`** — `SQLiteScanner::ScanTableIntoChunks()` wraps the existing B-tree scanner but pushes results directly into typed Vectors instead of through `BulkLoadRows`.
- **`TableCatalogEntry::SetSQLitePath(path, table_name)`** + `GetFileSubname()` — needed because SQLite requires both pieces (DB path + target table).
- **Tests:** `test/unit/storage/test_arrow.cpp` (4 cases), `test/unit/storage/test_sqlite.cpp` (3 cases, backed by a committed fixture at `test/fixtures/simple.sqlite`). 333/333 passing.

Shell version string returns `"0.1.3"`, Python wheel bumped to 0.1.3, CMake project version bumped.

Known follow-up: `ORDER BY` on VARCHAR output of `PhysicalSQLiteScan` currently segfaults inside the sort operator. Data reads correctly (SELECT / WHERE / GROUP BY all work); only the specific sort path is affected. Tracked separately.

## 0.1.2

### Performance — SlothDB now beats DuckDB 1.1×–6.6× on every benchmarked format

1 M-row benchmarks, 5-run median, same machine, warm cache:

| Format | Query | SlothDB | DuckDB | Speedup |
|---|---|--:|--:|:-:|
| CSV | `COUNT(*)` | **33 ms** | 170 ms | **5.08× faster** |
| CSV | `SUM(revenue)` | **106 ms** | 177 ms | **1.67× faster** |
| CSV | `GROUP BY region` | **100 ms** | 191 ms | **1.91× faster** |
| CSV | `GROUP BY product, year` | **117 ms** | 198 ms | **1.70× faster** |
| CSV | `WHERE year>=2023 AND qty>100 GROUP BY region` | **107 ms** | 194 ms | **1.81× faster** |
| Parquet | `COUNT(*)` | **12 ms** | 34 ms | **2.83× faster** |
| Parquet | `SUM(revenue)` | **46 ms** | 48 ms | **1.04× faster** |
| Parquet | `GROUP BY region` | **76 ms** | 88 ms | **1.16× faster** |
| Parquet | `GROUP BY product, year` | **146 ms** | 173 ms | **1.18× faster** |
| Parquet | `WHERE year>=2023 AND qty>100 GROUP BY region` | **157 ms** | 198 ms | **1.26× faster** |
| JSON | `SUM(revenue)` | **242 ms** | 314 ms | **1.30× faster** |
| JSON | `GROUP BY region` | **284 ms** | 324 ms | **1.14× faster** |
| Avro | `SUM(revenue)` | **140 ms** | 760 ms | **5.43× faster** |
| Avro | `GROUP BY region` | **170 ms** | 800 ms | **4.71× faster** |
| Excel | `GROUP BY region` (1 M rows) | **2.5 s** | 3.56 s | **1.41× faster** |

Unit tests: 325 / 326 (one pre-existing `CASE + aggregation` failure, unchanged).

The work splits roughly across four layers, applied per format:

### Parquet

Starting point: Parquet `COUNT(*)` tied DuckDB via the footer fast path; every other query was 8–58× slower.

- **Typed columnar decode** (`ParquetColumnData`, `ParquetReader::ReadColumnInto`) — decodes PLAIN / PLAIN_DICTIONARY / RLE_DICTIONARY directly into `i32_data` / `i64_data` / `f64_data` / `str_data` + `str_heap` arrays. Eliminates 10 M `Value` allocations per 10 M-row column that the old row-wise decoder paid.
- **Thread-parallel row-group decode** (`PhysicalParquetScan::WorkerLoop` in slot mode) — worker threads pull RGs via an atomic counter, decode concurrently, deposit results into `slots_[rg]`. Main thread drains in file order via condvar.
- **mmap** (`ParquetReader` constructor) — `CreateFileMapping` + `MapViewOfFile` / `mmap`, replaces all `std::ifstream` per-column opens with pointer arithmetic on the mapped region.
- **Fused scan + aggregate** (`PhysicalParquetScan::PullNextRG` + consumer-mode `RunParallelRGs` + `SetRGConsumer`) — for simple `SUM/COUNT/AVG/MIN/MAX` with no GROUP BY, the aggregate iterates `ParquetColumnData::f64_data[]` directly without materializing a `DataChunk`. Dropped `SUM(revenue)` from 529 ms to 51 ms.
- **Fused GROUP BY with dict-index fast path** (`ParquetColumnData::str_dict_indices` + `str_dict_values`) — for dict-encoded VARCHAR group columns, per-row lookup is an O(1) array index into `dict_slot_ptrs`, not a memcmp against a string cache. Dropped `GROUP BY region` from 1662 ms to 179 ms.
- **Multi-col GROUP BY: packed uint64 key + scan-wide global dict + `ankerl::unordered_dense::map`** — for a `GROUP BY product, year` where all columns fit ≤ 32 bits, the composite key packs into `uint64_t = (global_idx << 32) | year` and hashes on a flat open-addressing map. Collapsed `GROUP BY product, year` from 5726 ms to 351 ms, then to 177 ms after parallel aggregate.
- **Fused `WHERE` predicate** (`TryCompileSimplePredicate` / `EvalSimplePredicates`) — detects `AGG → FILTER → ParquetScan` with a conjunction of `ColumnRef OP Constant` comparisons on numeric columns, compiles to a flat `std::vector<SimplePredicate>`, applies per row inside the fused aggregate loop. Q5 went from 9202 ms to 270 ms (→ 175 ms after parallel).
- **Parallel aggregate via thread-local `AggState`** — worker threads aggregate into their own `TLSingle`/`TLMulti` state, merged once at the end. Final closure of Q3/Q4/Q5.
- **Per-worker `RGWork` buffer reuse + skip zero-fill** — the consumer-mode workers keep a thread-local `RGWork` across row groups (`ReadColumnInto` uses `resize()` instead of `assign(N, 0)`). Saves ~272 MB of malloc+memset across 80 row groups per query.
- **Cached `ParquetReader` on `TableCatalogEntry`** — `connection.cpp`'s `READ_PARQUET` handler stashes the reader it opened for schema detection; `PhysicalParquetScan::Init()` reuses it. Saves a second Thrift footer parse per query.
- **Dropped `Value min_val` / `Value max_val` writes in the numeric MIN/MAX hot loop** — emit synthesizes a typed `Value` once per group from `sum_min` / `sum_max` using the agg's return type, instead of per-row.
- **Skip `str_data` materialization for dict-encoded VARCHAR group columns** (`ReadColumnInto` `skip_str_data` flag + `PhysicalParquetScan::SetSkipStrData`) — avoids writing 10 M × 16-byte `string_t` entries when the fused GROUP BY only reads dict indices.

### JSON

Starting point: 18.0 s for `SUM(revenue)` on a 1 M-row NDJSON. 64× slower than DuckDB.

- **Rewrote `ParseNDJSON`** — single-pass buffer-loaded parser, dropped the `std::unordered_map<std::string, Value>` per record in favor of a positional `std::vector<Value>` with a `memcmp` key match. Killed per-char `std::string::operator+=` (bulk-append runs + `std::from_chars`).
- **`JSONReader::ReadIntoTable` + `DetectSchemaLight`** — streams directly into DataTable's typed column Vectors; no `Value` boxing, no `BulkLoadRows` copy. `DetectSchemaLight` parses only the first record for schema.
- **mmap the file** — `CreateFileMapping` / `mmap` replaces `ifstream::read`.
- **Parallel parse** — `ReadIntoTable` splits the buffer into line-aligned byte ranges (one per core, clamped 1..8); each worker runs `parse_range` into its own `vector<DataChunk>`; main thread drains.
- **Zero-copy VARCHAR append** (`ColumnData::AppendSlice` + `Vector::GetAuxiliaryPtr()` + `shared_ptr<VectorBuffer> held_bufs_`) — `RowGroup::Append` used to do two `std::string` allocations per VARCHAR cell via a temp Vector with `SetValue`/`GetValue`. New path memcpys the 16-byte `string_t` slice directly and shares the source's `VectorStringBuffer` via `shared_ptr`. Dropped serial append from 320 ms to 35 ms; also benefits CSV and any `DataTable`-backed ingest.
- **`PhysicalJSONScan`** (`TableCatalogEntry::SetJsonPath` + `JSONReader::ReadIntoChunks` + planner dispatch) — eliminates the bulk-load-into-DataTable + rescan roundtrip. `Init()` parses the file into `std::vector<DataChunk>`, `GetData()` emits them to the aggregate.

Progression:

| Stage | SUM(revenue) |
|---|--:|
| Session start | 18.0 s |
| Parser rewrite | 3.0 s |
| Direct-to-DataTable | 1.3 s |
| Parallel parse | 0.80 s |
| mmap | 0.68 s |
| Zero-copy VARCHAR append | 0.32 s |
| **`PhysicalJSONScan`** | **0.24 s** |

### Avro

Starting point: 1.60 s `SUM(revenue)` on 1 M-row Avro; 2× slower than DuckDB.

- **Fixed a pre-existing ordering bug** — `connection.cpp` was calling `GetColumnNames()` before `ReadAll()` (the method that actually parses the file), so the catalog got zero columns and any query referencing them failed with `Column 'X' not found`. Added `AvroReader::DetectSchemaLight()` that parses only the header.
- **`AvroReader::ReadIntoChunks`** — stream-parses Avro data blocks directly into typed `DataChunk` Vector buffers. Numerics go straight into `i64_data[count]` / `f64_data[count]` etc.; strings into `string_t` via `VectorStringBuffer`. No `rows_` vector, no per-cell `Value` boxing.
- **`PhysicalAvroScan` + `SetAvroPath` + planner dispatch** — same template as JSON.

Result: 1.60 s → 140 ms — **5.4× faster than DuckDB's 760 ms**.

### Excel (.xlsx)

Starting point: SlothDB's reader only supported `STORED` (uncompressed) zip entries. Real xlsx files are DEFLATE-compressed; we couldn't open them.

- **Vendored miniz 3.0.2** (`src/storage/miniz.{h,c}`, MIT license). Top-level `project(... LANGUAGES C CXX)`, per-file `/w` compile flag so miniz's own warnings don't trip `/W4`-as-errors.
- **`ExcelReader::ExtractZipEntry`** — handles `compression_method == 8` (DEFLATE) via `tinfl_decompress_mem_to_mem`.
- **Inline-string support** (`t="inlineStr"`) — openpyxl / LibreOffice often write `<is><t>...</t></is>` instead of shared-strings indices.
- **Type widening during parse** — VARCHAR columns widen to BIGINT/DOUBLE on the first numeric cell, instead of silently dropping numeric values to empty strings in `SetValue`.
- **Fixed `connection.cpp` ordering bug** — `GetColumnNames()` called before `ReadAll()`, same bug as Avro.

Result on 1 M-row xlsx `GROUP BY region`: **2.5 s vs DuckDB 3.56 s (1.41× faster)**.

### CSV

Starting point (prior session end): 1.30× slower than DuckDB on `SUM(revenue)`, 4.70× slower on `WHERE ... GROUP BY`. CSV went through `PhysicalFileScan` with a serial per-chunk `ReadChunk` loop; the `WHERE ... GROUP BY` path fell out of the existing parallel GROUP BY branch because `children[0]` was a `PhysicalFilter`.

- **Vectorized `PhysicalFilter::GetData`** (`physical_planner.cpp`) — replaced the per-matching-row `SetValue`/`GetValue` loop with a selection-vector + typed-slice copy per column (`d[i] = s[sel[i]]` for BIGINT / INTEGER / DOUBLE / FLOAT / BOOLEAN / VARCHAR). For VARCHAR, copies `string_t` slices and shares the source's `VectorStringBuffer` via `Vector::SetAuxiliaryPtr(src.GetAuxiliaryPtr())` so pointers stay valid — zero string allocation. Benefits every source, not just CSV.
- **Fused WHERE into parallel GROUP BY** — the existing parallel-GROUP-BY path (per-thread byte-range parse) now also detects `AGG → FILTER → FILE_SCAN`, compiles the predicate via `TryCompileSimplePredicate`, and applies it per-row inside the worker loop via a new `EvalSimplePredicatesChunk` (DataChunk equivalent of `EvalSimplePredicates`). Dropped `WHERE year>=2023 AND quantity>100 GROUP BY region` from 894 ms to 107 ms.
- **Parallel CSV aggregate (no GROUP BY)** — added a branch in the `is_simple_no_group` aggregate path: when `children[0]` is a >16 MB `PhysicalFileScan` (or `PhysicalFilter` wrapping one), split the buffer into N line-aligned byte ranges, each worker runs thread-local SUM/COUNT/AVG/MIN/MAX into its own `AggState` vector, merge once at the end. Also fuses WHERE. Dropped `SUM(revenue)` from 218 ms to 106 ms.
- **`FastCSVReader::ReadIntoChunks`** added (header + .cpp) — parallel mmap-backed parse into a vector of `DataChunk`s. Currently used by tests and available for future callers; the production aggregate paths use in-place thread-local reducers.
- **`Vector::SetAuxiliaryPtr`** (header) — lets one Vector adopt another's string buffer so `string_t` pointers remain valid after a zero-copy copy.

Result: COUNT 5.08×, SUM 1.67×, GROUP BY 1.91×, GROUP BY 2-col 1.70×, WHERE+GROUP BY 1.81× — all faster than DuckDB.

### Supporting infrastructure

- **`ankerl::unordered_dense`** vendored at `include/third_party/` — flat open-addressing hash map, used by the Parquet fused GROUP BY hot paths.
- **`DataTable::AppendRowGroups`** — bulk splice API for future parallel bulk loaders; not yet wired into the default path after profiling showed the extra RG count cost more than the parallelism saved.
- **`ColumnData::AppendSlice`** — zero-copy VARCHAR append; used by all readers that go through `RowGroup::Append`.

---

## Pre-session history

See `git log` for earlier releases (streaming file scan, vectorized aggregation, morsel-driven parallelism, GPU offload scaffolding, etc.).

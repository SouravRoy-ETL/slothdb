# SlothDB Edge Build

Sub-megabyte SlothDB for Cloudflare Workers, Deno Deploy, Vercel Edge, and any browser context where bundle size matters more than format breadth.

## Who this is for

You want analytical SQL over a CSV, JSON, or Parquet file fetched via HTTP, inside a runtime that caps bundle size at ~1 MB gzipped. The full `@slothdb/wasm` build (1.3 MB WASM) exceeds the Cloudflare Workers 1 MB Worker-script-size limit. `@slothdb/wasm-edge` is the surgical build: CSV + JSON + Parquet only, Excel / Avro / Arrow IPC / SQLite scanner stripped.

## What it includes

- CSV / TSV readers (FastCSVReader, vectorized filter, fused WHERE)
- JSON / NDJSON / JSONL reader
- Parquet reader (Thrift + Snappy + PLAIN + PLAIN_DICTIONARY + RLE)
- Full SQL engine: joins, CTEs, window functions, QUALIFY, MERGE, aggregates
- HTTP fetch for file inputs

## What it excludes

- Excel (`read_xlsx`) — strips the miniz ZIP dependency
- Avro (`read_avro`)
- Arrow IPC (`read_arrow`)
- SQLite scanner (`sqlite_scan`)

Calling any excluded reader throws a clear `BinderException` pointing at the full build. `SELECT * FROM 'foo.xlsx'` fails with `Unknown file format: .xlsx (edge build supports CSV / JSON / Parquet only)` at parse time.

## How to build

```bash
cmake -B build -DSLOTHDB_EDGE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Or through Emscripten:

```bash
emcmake cmake -B build-wasm-edge -DSLOTHDB_EDGE=ON
cmake --build build-wasm-edge
```

The Emscripten path passes `-Oz` (size optimization), `-sMALLOC=emmalloc` (smaller heap allocator), `-sFILESYSTEM=0` (no bundled FS — use `fetch()` + `ArrayBuffer`), and `--closure=1` to minify the JS glue.

## Expected size

On MSVC/Windows the native binary shrinks modestly (~6%), because the linker already strips dead code. On Emscripten with `-Oz` and closure minification, the win is larger — target is **under 1 MB gzipped** for the combined `.wasm` + JS glue. Validate with:

```bash
ls -la build-wasm-edge/src/slothdb.wasm
wc -c < build-wasm-edge/src/slothdb.js
```

Add a CI gate that fails if gzip size exceeds the Cloudflare cap.

## Runtime distinctions

The edge build does not embed an emscripten filesystem (`FILESYSTEM=0`). Read file data via `fetch()` and pass an `ArrayBuffer` through the JS bindings. The CLI shell's `.open path.csv` semantics don't apply in this build mode.

# SlothDB Playground

Live: **https://slothdb.org/playground/**

SQL against CSV / Parquet / JSON / Arrow / SQLite / Excel in the browser. No install, no upload — the WASM build runs the full SlothDB engine on the page and files live in MEMFS.

## Using the playground

UI borrows VS Code / pgAdmin conventions:

- **Activity bar** (far left) — Explorer · Snippets · History · About.
- **Sidebar** — file tree (`/data/sales.csv`, `/data/sales.parquet` are preloaded) and inline snippets. Click a file to insert its path at the cursor. Click a snippet to load + auto-run.
- **Editor** — CodeMirror 6 with SQL highlighting, line numbers, bracket matching, Ctrl+F search, full undo/redo.
- **Results panel** — sortable grid (click any header), sticky columns, CSV export.
- **Status bar** — connection status, SlothDB version, row count, query time.

**Keys:** `Ctrl+Enter` run · `Ctrl+F` find · `Ctrl+/` toggle comment · `Tab` indent.

The preloaded demo contains the same 1,000 rows in both CSV and Parquet form (generated with the same LCG seed) so you can verify format-parity with the "CSV vs Parquet" snippet.

## Architecture

- `tools/playground/wasm_bindings.cpp` — embind surface over the C API: `openDatabase()`, `runQuery(sql) → JSON`, `version()`.
- `src/storage/http_client.cpp` — `__EMSCRIPTEN__` branch stubs HTTP to an error ("load files from the browser instead"). MEMFS handles `fopen`/`fread` natively.
- `src/CMakeLists.txt` — adds `slothdb_wasm` target gated on `EMSCRIPTEN`, builds with `-fexceptions -O3`, outputs ES6 module via embind.
- `docs/playground/` — static site (HTML + CSS + `app.js` + `vendor/cm.js` + `slothdb.{js,wasm}` + `data/sales.{csv,parquet}`). Committed artifacts = drop-in GH Pages deploy, no CI build step.

## Deploy (GitHub Pages)

This repo already serves `docs/` via GitHub Pages at `slothdb.org` (see `docs/CNAME`). Once committed and pushed, the playground goes live at:

**https://slothdb.org/playground/**

No build step in CI — all artifacts (`slothdb.wasm`, `slothdb.js`, `vendor/cm.js`, demo data) are committed under `docs/playground/`. GitHub Pages serves `.wasm` with `application/wasm` by default.

## Local preview

```bash
cd docs/playground
python -m http.server 8765
# open http://localhost:8765
```

Python 3.10+ serves `.wasm` with the right MIME type out of the box. No threads, no COOP/COEP — single-origin, single-thread.

## Rebuilding the demo data

```bash
python docs/playground/data/generate.py
# regenerates sales.csv + sales.parquet deterministically
```

The CSV and Parquet files are generated from the same LCG seed, so `SUM(revenue)` matches byte-for-byte between them. Use snippet "CSV vs Parquet" to verify.

## Rebuilding the CodeMirror bundle

The editor uses CodeMirror 6, bundled locally into `vendor/cm.js` to avoid CDN version-drift and multi-instance issues with `@codemirror/state`.

```bash
mkdir -p /tmp/cm-bundle && cd /tmp/cm-bundle
cat > entry.js <<'EOF'
export { EditorView, basicSetup } from 'codemirror';
export { keymap } from '@codemirror/view';
export { EditorState } from '@codemirror/state';
export { indentWithTab } from '@codemirror/commands';
export { sql } from '@codemirror/lang-sql';
export { oneDark } from '@codemirror/theme-one-dark';
EOF
npm init -y >/dev/null && npm pkg set type=module
npm install codemirror@6.0.1 @codemirror/view@6.41.1 @codemirror/state@6.6.0 \
            @codemirror/commands@6.10.3 @codemirror/lang-sql@6.8.0 \
            @codemirror/theme-one-dark@6.1.2 esbuild
npx esbuild entry.js --bundle --format=esm --minify --target=es2022 \
    --outfile=<repo>/docs/playground/vendor/cm.js
```

All six packages must share a single `@codemirror/state` — the pins above are known-compatible. Double-check with `npm ls @codemirror/state` before bundling.

## Rebuilding the WASM

Requires [emsdk](https://github.com/emscripten-core/emsdk) activated in the shell.

```bash
./scripts/build-wasm.sh
cp build-wasm/src/slothdb.js   docs/playground/slothdb.js
cp build-wasm/src/slothdb.wasm docs/playground/slothdb.wasm
```

Single-threaded build (no `-pthread`). Expect roughly 1.3 MB `.wasm` + 100 KB `.js` at `-O3`.

## How it works

- `tools/playground/wasm_bindings.cpp` wraps the C API in three embind functions: `openDatabase()`, `runQuery(sql) → JSON`, `version()`.
- `app.js` boots the module, fetches demo data into MEMFS at `/data/sales.{csv,parquet}`, and wires the VS Code-style UI.
- File uploads write straight into MEMFS at `/data/<filename>`. Queries reference them with the same path: `SELECT * FROM '/data/mydata.csv'`.

## Known limits

- **HTTP reads are stubbed.** `SELECT FROM 'https://…'` returns an error. Use the file picker instead.
- **Single-threaded.** Parallel CSV / Parquet / JSON scanners take the serial path. Fine at playground scale.
- **Max memory 2 GB.** Grows on demand. Browsers cap WASM at ~4 GB; we're well under.

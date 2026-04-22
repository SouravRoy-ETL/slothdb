# @slothdb/wasm

> Fast embedded SQL database in WebAssembly. Query Parquet, CSV, JSON, Arrow, SQLite, and Excel files directly from SQL — no server, no install, 1.3 MB wasm.

[![npm](https://img.shields.io/npm/v/@slothdb/wasm?color=CB3837&logo=npm)](https://www.npmjs.com/package/@slothdb/wasm)
[![License](https://img.shields.io/badge/license-MIT-blue)](https://github.com/SouravRoy-ETL/slothdb/blob/main/LICENSE)

- **Website:** https://slothdb.org
- **Try it live:** https://slothdb.org/playground
- **GitHub:** https://github.com/SouravRoy-ETL/slothdb

SlothDB is a full analytical SQL engine compiled to WebAssembly. It reads seven file formats natively, runs in Node ≥18 and every modern browser, and runs **1.1×–8.6× faster than DuckDB** on the benchmarked formats in its native build. The WASM build is single-threaded; expect browser-scale latencies, not native-scale.

## Install

```bash
npm install @slothdb/wasm
```

No native dependencies. Works on Linux, macOS, Windows (and WSL).

## Quick start

```js
import { SlothDB } from '@slothdb/wasm';
import fs from 'node:fs/promises';

const db = await SlothDB.create();

// Load a file into the virtual filesystem:
const parquet = await fs.readFile('./sales.parquet');
db.loadFile('/data/sales.parquet', parquet);

// Query it:
const { columns, rows, ms } = db.query(`
    SELECT region, SUM(revenue) AS total
    FROM '/data/sales.parquet'
    GROUP BY region
    ORDER BY total DESC
`);

console.log(columns);  // ['region', 'total']
console.log(rows);     // [['EU', '14184859'], ['NA', '12295714'], ...]
console.log(`ran in ${ms.toFixed(1)} ms`);
```

## File formats supported

All seven formats work directly from SQL by file path — no `CREATE TABLE`, no `COPY FROM`.

| Format | Example |
|---|---|
| Parquet | `SELECT * FROM '/data.parquet'` |
| CSV / TSV | `SELECT * FROM '/data.csv'` |
| JSON / NDJSON | `SELECT * FROM '/data.json'` |
| Apache Arrow IPC | `SELECT * FROM '/data.arrow'` |
| Avro | `SELECT * FROM '/data.avro'` |
| SQLite | `SELECT * FROM sqlite_scan('/app.db', 'users')` |
| Excel (.xlsx) | `SELECT * FROM '/report.xlsx'` |

## SQL features

SELECT, WHERE, GROUP BY, HAVING, ORDER BY, LIMIT, OFFSET. Joins (INNER / LEFT / RIGHT / FULL / NATURAL / USING). CTEs including recursive. Window functions with QUALIFY. Set operations (UNION / INTERSECT / EXCEPT). MERGE. CAST. 70+ scalar functions — string, math, date/time, regex. DuckDB-compatible `DATE_TRUNC` with all intervals (MICROSECOND through MILLENNIUM), plus `MONTHNAME`, `DAYNAME`, `LAST_DAY`, `MAKE_DATE`.

Full SQL reference: https://slothdb.org/docs.html

## Browser usage

SlothDB works the same in the browser — the only differences are that you load files via `fetch`/`<input type="file">` instead of `fs`, and the package needs to be served over HTTP (not `file://`).

```js
import { SlothDB } from '@slothdb/wasm';

const db = await SlothDB.create();

const response = await fetch('/data/sales.parquet');
const buffer = new Uint8Array(await response.arrayBuffer());
db.loadFile('/data/sales.parquet', buffer);

const result = db.query("SELECT COUNT(*) FROM '/data/sales.parquet'");
```

With Vite / webpack / esbuild, `slothdb.wasm` is loaded automatically alongside `slothdb.js` via the bundler's asset handling. No special config required for the common cases; see https://slothdb.org/docs.html for bundler-specific notes.

## API

### `SlothDB.create(options?)`

Boot a new in-memory database. Returns `Promise<SlothDB>`.

- `options.moduleOptions` — pass-through to the underlying Emscripten `Module` factory. Rarely needed.

### `db.query(sql)`

Execute a SQL statement. Returns:

```ts
{
    columns: string[];
    rows: (string | null)[][];
    ms: number;         // engine-side execution time
    rowCount: number;
}
```

All non-NULL values come back as strings to preserve full precision. Cast at the JS layer (`Number(v)`, `BigInt(v)`) as needed.

Throws `Error` on SQL error; the thrown error includes `.ms` and `.sql` properties for diagnostics.

### `db.loadFile(path, data)`

Write a byte buffer (or string) to a virtual path. Accepts `Uint8Array`, `ArrayBuffer`, or `string`. Absolute paths required (starting with `/`).

### `db.readFile(path)`

Read a file back from the virtual filesystem. Returns `Uint8Array`.

### `db.version()`

Returns the engine version string, e.g. `"0.1.5"`.

## Limitations of the WASM build

- **Single-threaded.** Native SlothDB uses parallel readers; the WASM build does not (SharedArrayBuffer/COOP-COEP overhead isn't worth it for playground-scale workloads).
- **No HTTP reads.** `FROM 'https://example.com/data.csv'` returns an error. Fetch the file in JS and load it via `loadFile` instead.
- **Memory cap.** Starts at 64 MB, grows to a 2 GB hard cap. Plenty for most analytical workloads; too small for a 10 GB Parquet.

For the full feature set (threading, HTTP reads, extensions) use the native library: https://slothdb.org.

## License

MIT. Copyright (c) 2026 Sourav Roy. See [LICENSE](./LICENSE).

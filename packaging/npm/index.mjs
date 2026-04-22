// SlothDB — a fast embedded SQL database compiled to WebAssembly.
// Public JS API that wraps the embind surface.
//
// Usage:
//   import { SlothDB } from 'slothdb';
//   const db = await SlothDB.create();
//   const { columns, rows, ms } = db.query("SELECT 1 AS n");
//
// File I/O uses Emscripten's MEMFS. Load a byte buffer to a virtual path,
// then query the same path from SQL:
//   await db.loadFile('/data/sales.parquet', buffer);
//   const r = db.query("SELECT region, SUM(revenue) FROM '/data/sales.parquet' GROUP BY region");

import createSlothDBModule from './slothdb.js';

export class SlothDB {
    /** @private */
    constructor(_internal) {
        this._mod = _internal.mod;
    }

    /**
     * Boot a new SlothDB instance backed by a WASM module.
     * Resolves once the database is open and ready for queries.
     *
     * @param {object} [options]
     * @param {object} [options.moduleOptions] — pass-through to Emscripten module init.
     * @returns {Promise<SlothDB>}
     */
    static async create(options = {}) {
        const mod = await createSlothDBModule({
            // Resolve slothdb.wasm relative to this module's URL so the package
            // works in Node (file://) and browsers (http://).
            locateFile: (p) => new URL(p, import.meta.url).href,
            ...(options.moduleOptions || {}),
        });
        mod.openDatabase();
        return new SlothDB({ mod });
    }

    /**
     * Execute a SQL statement. Returns a result object. Throws on query error.
     *
     * @param {string} sql
     * @returns {{ columns: string[], rows: (string|null)[][], ms: number, rowCount: number }}
     */
    query(sql) {
        if (typeof sql !== 'string') {
            throw new TypeError('SlothDB.query(sql): sql must be a string');
        }
        const json = this._mod.runQuery(sql);
        const result = JSON.parse(json);
        if (result.error) {
            const err = new Error(result.error);
            err.ms = result.ms;
            err.sql = sql;
            throw err;
        }
        return result;
    }

    /**
     * Write a file into the virtual filesystem. The same path can then be
     * referenced from SQL: FROM '/your/path.parquet'.
     *
     * @param {string} memfsPath — absolute path, e.g. '/data/sales.csv'
     * @param {Uint8Array | ArrayBuffer | string} data
     */
    loadFile(memfsPath, data) {
        if (typeof memfsPath !== 'string' || !memfsPath.startsWith('/')) {
            throw new Error("SlothDB.loadFile: memfsPath must be an absolute path like '/data/file.csv'");
        }
        const parent = memfsPath.substring(0, memfsPath.lastIndexOf('/'));
        if (parent) {
            try { this._mod.FS.mkdirTree(parent); } catch (_) { /* already exists */ }
        }
        let bytes;
        if (data instanceof Uint8Array) bytes = data;
        else if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
        else if (typeof data === 'string') bytes = new TextEncoder().encode(data);
        else throw new TypeError('SlothDB.loadFile: data must be Uint8Array, ArrayBuffer, or string');

        this._mod.FS.writeFile(memfsPath, bytes);
    }

    /**
     * Read a file back from the virtual filesystem.
     *
     * @param {string} memfsPath
     * @returns {Uint8Array}
     */
    readFile(memfsPath) {
        return this._mod.FS.readFile(memfsPath);
    }

    /**
     * Return the underlying SlothDB engine version string.
     *
     * @returns {string}
     */
    version() {
        return this._mod.version();
    }
}

export default SlothDB;

// Type definitions for the slothdb npm package.

export interface QueryResult {
    /** Column names in output order. */
    columns: string[];
    /** Rows, each row is an array of string (or null) values ordered by columns. */
    rows: (string | null)[][];
    /** Execution time in milliseconds (server-side, excludes JS overhead). */
    ms: number;
    /** Number of rows returned. */
    rowCount: number;
}

export interface CreateOptions {
    /** Pass-through options forwarded to the Emscripten Module factory. */
    moduleOptions?: Record<string, unknown>;
}

export declare class SlothDB {
    private constructor(_internal: unknown);

    /**
     * Boot a new SlothDB instance backed by a WASM module.
     * The returned instance is ready to run queries.
     */
    static create(options?: CreateOptions): Promise<SlothDB>;

    /**
     * Execute a SQL statement. Throws on query error.
     * Numeric values come back as strings (full precision preserved).
     */
    query(sql: string): QueryResult;

    /**
     * Write a file into the virtual filesystem. Reference it from SQL
     * via the same path: `FROM '/data/sales.parquet'`.
     */
    loadFile(
        memfsPath: string,
        data: Uint8Array | ArrayBuffer | string,
    ): void;

    /** Read a file back from the virtual filesystem. */
    readFile(memfsPath: string): Uint8Array;

    /** Engine version string, e.g. "0.1.5". */
    version(): string;
}

export default SlothDB;

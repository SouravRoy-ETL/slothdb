"""
SlothDB - An embedded analytical database engine.

Usage:
    import slothdb

    # In-memory database
    db = slothdb.connect()

    # Persistent database
    db = slothdb.connect("my_data.slothdb")

    # Run queries
    result = db.sql("SELECT 42 AS answer")
    print(result)

    # Query files directly
    result = db.sql("SELECT * FROM read_parquet('data.parquet')")
    result = db.sql("SELECT * FROM 'data.csv'")
"""

import ctypes
import os
import sys
import platform

__version__ = "0.2.6"


def _find_library():
    """Find the SlothDB shared library."""
    here = os.path.dirname(os.path.abspath(__file__))
    system = platform.system()

    if system == "Windows":
        names = ["slothdb.dll", "libslothdb.dll"]
    elif system == "Darwin":
        names = ["libslothdb.dylib", "slothdb.dylib"]
    else:
        names = ["libslothdb.so", "slothdb.so"]

    search_paths = [
        here,
        os.path.join(here, "lib"),
        os.path.join(here, "..", "lib"),
        os.path.join(here, "..", "build", "src", "Release"),
        os.path.join(here, "..", "build", "src"),
        os.path.join(here, "..", "..", "build", "src", "Release"),
        os.path.join(here, "..", "..", "build", "src"),
    ]

    for path in search_paths:
        for name in names:
            full = os.path.join(path, name)
            if os.path.exists(full):
                return full

    return None


def _load_library():
    """Load the SlothDB C library."""
    lib_path = _find_library()

    if lib_path is None:
        # Last-ditch attempt at the OS loader's default search path.
        try:
            if platform.system() == "Windows":
                return ctypes.CDLL("slothdb.dll")
            else:
                return ctypes.CDLL("libslothdb.so")
        except OSError:
            pass

        system = platform.system()
        machine = platform.machine()
        here = os.path.dirname(os.path.abspath(__file__))
        raise ImportError(
            f"SlothDB native library not found.\n"
            f"  Platform: {system} {machine}, Python {sys.version.split()[0]}\n"
            f"  Searched: {here} (and ../lib, ../build/src/Release)\n"
            f"\n"
            f"  This usually means pip installed a wheel that doesn't match\n"
            f"  your platform. Try:\n"
            f"    pip install --upgrade --force-reinstall slothdb\n"
            f"\n"
            f"  Or build from source:\n"
            f"    git clone https://github.com/SouravRoy-ETL/slothdb\n"
            f"    cd slothdb && cmake -B build -DSLOTHDB_BUILD_SHARED=ON\n"
            f"    cmake --build build --config Release\n"
            f"\n"
            f"  If neither works, please file an issue with this error\n"
            f"  message at https://github.com/SouravRoy-ETL/slothdb/issues"
        )

    return ctypes.CDLL(lib_path)


class QueryResult:
    """Result of a SQL query.

    Three materialisation paths, all lazy:

      * `.rows`           list of tuples         (best for small results, dict-like access)
      * `.fetchnumpy()`   dict of numpy arrays   (zero-copy for numeric columns)
      * `.fetchdf()`      pandas DataFrame       (built from fetchnumpy, fast for big results)

    The underlying C result is held alive until this object is garbage
    collected; both .rows and .fetchnumpy() can be called on the same
    QueryResult without re-querying.
    """

    # SLOTHDB_TYPE_* mirror src/api/slothdb.h.
    TYPE_BOOLEAN, TYPE_INTEGER, TYPE_BIGINT = 2, 5, 6
    TYPE_FLOAT, TYPE_DOUBLE, TYPE_VARCHAR = 11, 12, 15

    def __init__(self, lib, result_ptr, columns, col_types, num_rows, has_batch_api):
        self._lib = lib
        self._result_ptr = result_ptr
        self.columns = columns
        self._col_types = col_types
        self._num_rows = num_rows
        self._has_batch_api = has_batch_api
        self._rows_cache = None
        self._numpy_cache = None

    def __del__(self):
        self.close()

    def close(self):
        """Free the underlying C result. Called automatically on GC."""
        if self._lib is not None and self._result_ptr is not None:
            self._lib.slothdb_free_result(self._result_ptr)
        self._lib = None
        self._result_ptr = None

    @property
    def column_names(self):
        return [c["name"] for c in self.columns]

    @property
    def column_count(self):
        return len(self.columns)

    @property
    def row_count(self):
        return self._num_rows

    @property
    def rows(self):
        """Row tuples, materialised on first access. List of tuples."""
        if self._rows_cache is None:
            self._rows_cache = self._materialise_rows()
        return self._rows_cache

    def fetchone(self):
        rows = self.rows
        return rows[0] if rows else None

    def fetchall(self):
        return self.rows

    def fetchnumpy(self):
        """Return columns as a dict of numpy arrays.

        Numeric columns (INTEGER / BIGINT / FLOAT / DOUBLE) are zero-copy
        wraps over the C buffers (via np.frombuffer + a defensive copy
        so the array stays valid after the QueryResult is freed). VARCHAR
        becomes a numpy object array of Python strings. 10M-row int
        columns: drops conversion from ~4 s (list of c_int64 -> Python
        ints) to ~50 ms.
        """
        if self._numpy_cache is None:
            self._numpy_cache = self._materialise_numpy()
        return self._numpy_cache

    def fetchdf(self):
        """Convert to pandas DataFrame. Built from fetchnumpy(), so it
        skips the row-tuple materialisation cost. Requires pandas."""
        try:
            import pandas as pd
        except ImportError:
            raise ImportError("pandas is required for fetchdf()")
        cols = self.fetchnumpy()
        return pd.DataFrame(cols, columns=self.column_names)

    def __repr__(self):
        if not self.columns:
            return "QueryResult(empty)"
        header = " | ".join(c["name"] for c in self.columns)
        sep = "-+-".join("-" * max(len(c["name"]), 10) for c in self.columns)
        lines = [header, sep]
        for row in self.rows[:20]:
            line = " | ".join(str(v)[:10].ljust(max(len(c["name"]), 10))
                            for v, c in zip(row, self.columns))
            lines.append(line)
        if len(self.rows) > 20:
            lines.append(f"... ({self._num_rows} rows total)")
        return "\n".join(lines)

    def show(self):
        print(self.__repr__())

    def __len__(self):
        return self._num_rows

    # ---- private materialisers ---------------------------------------------

    def _materialise_numpy(self):
        try:
            import numpy as np
        except ImportError:
            raise ImportError("numpy is required for fetchnumpy()")
        result = {}
        n = self._num_rows
        if n == 0:
            for c in range(self.column_count):
                result[self.columns[c]["name"]] = np.array([])
            return result

        TYPE_BIGINT, TYPE_INTEGER = self.TYPE_BIGINT, self.TYPE_INTEGER
        TYPE_DOUBLE, TYPE_FLOAT = self.TYPE_DOUBLE, self.TYPE_FLOAT
        TYPE_VARCHAR = self.TYPE_VARCHAR

        for c in range(self.column_count):
            name = self.columns[c]["name"]
            t = self._col_types[c]
            v_ptr = self._lib.slothdb_column_validity_buffer(self._result_ptr, c)
            valid_arr = None
            if v_ptr:
                vbuf = (ctypes.c_uint8 * n).from_address(
                    ctypes.addressof(v_ptr.contents))
                valid_arr = np.frombuffer(vbuf, dtype=np.uint8).astype(bool)

            if t in (TYPE_BIGINT, TYPE_INTEGER):
                p = self._lib.slothdb_column_int64_buffer(self._result_ptr, c)
                if not p:
                    result[name] = np.zeros(n, dtype=np.int64); continue
                buf = (ctypes.c_int64 * n).from_address(ctypes.addressof(p.contents))
                arr = np.frombuffer(buf, dtype=np.int64).copy()
                if valid_arr is not None and not valid_arr.all():
                    arr = arr.astype(np.float64)
                    arr[~valid_arr] = np.nan
                result[name] = arr
            elif t in (TYPE_DOUBLE, TYPE_FLOAT):
                p = self._lib.slothdb_column_double_buffer(self._result_ptr, c)
                if not p:
                    result[name] = np.zeros(n, dtype=np.float64); continue
                buf = (ctypes.c_double * n).from_address(ctypes.addressof(p.contents))
                arr = np.frombuffer(buf, dtype=np.float64).copy()
                if valid_arr is not None and not valid_arr.all():
                    arr[~valid_arr] = np.nan
                result[name] = arr
            elif t == TYPE_VARCHAR:
                offs_pp = ctypes.POINTER(ctypes.c_uint64)()
                blob_p = ctypes.c_char_p()
                rc = self._lib.slothdb_column_varchar_buffer(
                    self._result_ptr, c, ctypes.byref(offs_pp), ctypes.byref(blob_p))
                if rc != 0:
                    result[name] = np.array([""] * n, dtype=object); continue
                offs = (ctypes.c_uint64 * (n + 1)).from_address(
                    ctypes.addressof(offs_pp.contents))
                blob_addr = ctypes.cast(blob_p, ctypes.c_void_p).value
                total = offs[n]
                if blob_addr is None or total == 0:
                    arr = np.array([""] * n, dtype=object)
                else:
                    whole = ctypes.string_at(blob_addr, total)
                    arr = np.empty(n, dtype=object)
                    for i in range(n):
                        arr[i] = whole[offs[i]:offs[i + 1]].decode(
                            "utf-8", errors="replace")
                if valid_arr is not None and not valid_arr.all():
                    arr = arr.astype(object)
                    for i, v in enumerate(valid_arr):
                        if not v: arr[i] = None
                result[name] = arr
            else:
                # Unknown type: fall back to per-cell varchar
                vals = []
                for r in range(n):
                    if self._lib.slothdb_value_is_null(self._result_ptr, r, c):
                        vals.append(None)
                    else:
                        s = self._lib.slothdb_value_varchar(self._result_ptr, r, c)
                        vals.append(s.decode("utf-8") if s else "")
                result[name] = np.array(vals, dtype=object)
        return result

    def _materialise_rows(self):
        """Build list-of-row-tuples from C buffers. Slow for large results;
        prefer fetchnumpy() / fetchdf() in that case."""
        n = self._num_rows
        if n == 0:
            return []

        # Use the typed-batch path when every column is in the fast set.
        FAST = {self.TYPE_INTEGER, self.TYPE_BIGINT, self.TYPE_DOUBLE,
                self.TYPE_FLOAT, self.TYPE_VARCHAR}
        all_fast = self._has_batch_api and all(t in FAST for t in self._col_types)

        if all_fast:
            cols_data = []
            cols_validity = []
            for c in range(self.column_count):
                t = self._col_types[c]
                v_ptr = self._lib.slothdb_column_validity_buffer(self._result_ptr, c)
                validity = None
                if v_ptr:
                    validity = (ctypes.c_uint8 * n).from_address(
                        ctypes.addressof(v_ptr.contents))
                if t in (self.TYPE_BIGINT, self.TYPE_INTEGER):
                    p = self._lib.slothdb_column_int64_buffer(self._result_ptr, c)
                    if not p: all_fast = False; break
                    arr = (ctypes.c_int64 * n).from_address(
                        ctypes.addressof(p.contents))
                    cols_data.append(list(arr))
                elif t in (self.TYPE_DOUBLE, self.TYPE_FLOAT):
                    p = self._lib.slothdb_column_double_buffer(self._result_ptr, c)
                    if not p: all_fast = False; break
                    arr = (ctypes.c_double * n).from_address(
                        ctypes.addressof(p.contents))
                    cols_data.append(list(arr))
                elif t == self.TYPE_VARCHAR:
                    offs_pp = ctypes.POINTER(ctypes.c_uint64)()
                    blob_p = ctypes.c_char_p()
                    rc = self._lib.slothdb_column_varchar_buffer(
                        self._result_ptr, c, ctypes.byref(offs_pp), ctypes.byref(blob_p))
                    if rc != 0: all_fast = False; break
                    offs = (ctypes.c_uint64 * (n + 1)).from_address(
                        ctypes.addressof(offs_pp.contents))
                    blob_addr = ctypes.cast(blob_p, ctypes.c_void_p).value
                    if blob_addr is None or offs[n] == 0:
                        cols_data.append([""] * n)
                    else:
                        whole = ctypes.string_at(blob_addr, offs[n])
                        cols_data.append([
                            whole[offs[i]:offs[i + 1]].decode("utf-8", errors="replace")
                            for i in range(n)
                        ])
                else:
                    all_fast = False; break
                cols_validity.append(validity)

            if all_fast:
                if any(v is not None for v in cols_validity):
                    rows = []
                    for r in range(n):
                        row = []
                        for c in range(self.column_count):
                            v = cols_validity[c]
                            if v is not None and not v[r]:
                                row.append(None)
                            else:
                                row.append(cols_data[c][r])
                        rows.append(tuple(row))
                else:
                    rows = list(zip(*cols_data))
                return rows

        # Fallback: per-cell varchar, used when types are mixed.
        rows = []
        for r in range(n):
            row = []
            for c in range(self.column_count):
                if self._lib.slothdb_value_is_null(self._result_ptr, r, c):
                    row.append(None)
                else:
                    val = self._lib.slothdb_value_varchar(self._result_ptr, r, c)
                    row.append(val.decode("utf-8") if val else "")
            rows.append(tuple(row))
        return rows


class Connection:
    """Connection to a SlothDB database."""

    def __init__(self, path=""):
        self._path = path
        self._db = None
        self._conn = None
        self._lib = None
        self._open()

    def _open(self):
        """Open the database connection using the C API."""
        # Surface the load error to the caller. The previous behaviour
        # silently swallowed ImportError and only raised a generic
        # "library not loaded" later from sql() — useless for debugging.
        self._lib = _load_library()

        # Set up C API function signatures
        lib = self._lib
        lib.slothdb_open.restype = ctypes.c_int
        lib.slothdb_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.slothdb_connect.restype = ctypes.c_int
        lib.slothdb_connect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.slothdb_query.restype = ctypes.c_int
        lib.slothdb_query.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.slothdb_column_count.restype = ctypes.c_uint64
        lib.slothdb_row_count.restype = ctypes.c_uint64
        lib.slothdb_column_name.restype = ctypes.c_char_p
        lib.slothdb_column_type.restype = ctypes.c_int
        lib.slothdb_value_varchar.restype = ctypes.c_char_p
        lib.slothdb_value_is_null.restype = ctypes.c_int
        lib.slothdb_value_int32.restype = ctypes.c_int32
        lib.slothdb_value_int64.restype = ctypes.c_int64
        lib.slothdb_value_double.restype = ctypes.c_double
        lib.slothdb_result_error.restype = ctypes.c_char_p
        lib.slothdb_free_result.argtypes = [ctypes.c_void_p]
        lib.slothdb_close.argtypes = [ctypes.c_void_p]
        lib.slothdb_disconnect.argtypes = [ctypes.c_void_p]
        # Typed batch fetch — one C call per column instead of two per cell.
        # Critical for SELECT N rows queries: drops materialisation cost
        # from O(rows * cols * 2 ctypes calls) to O(cols).
        try:
            lib.slothdb_column_int32_buffer.restype = ctypes.POINTER(ctypes.c_int32)
            lib.slothdb_column_int32_buffer.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
            lib.slothdb_column_int64_buffer.restype = ctypes.POINTER(ctypes.c_int64)
            lib.slothdb_column_int64_buffer.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
            lib.slothdb_column_double_buffer.restype = ctypes.POINTER(ctypes.c_double)
            lib.slothdb_column_double_buffer.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
            lib.slothdb_column_validity_buffer.restype = ctypes.POINTER(ctypes.c_uint8)
            lib.slothdb_column_validity_buffer.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
            lib.slothdb_column_varchar_buffer.restype = ctypes.c_int
            lib.slothdb_column_varchar_buffer.argtypes = [
                ctypes.c_void_p, ctypes.c_uint64,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_uint64)),
                ctypes.POINTER(ctypes.c_char_p),
            ]
            self._has_batch_api = True
        except AttributeError:
            self._has_batch_api = False

        # Open database
        self._db = ctypes.c_void_p()
        path_bytes = self._path.encode("utf-8") if self._path else b""
        status = lib.slothdb_open(path_bytes, ctypes.byref(self._db))
        if status != 0:
            raise RuntimeError(f"Failed to open database: {self._path}")

        # Create connection
        self._conn = ctypes.c_void_p()
        status = lib.slothdb_connect(self._db, ctypes.byref(self._conn))
        if status != 0:
            raise RuntimeError("Failed to create connection")

    def sql(self, query):
        """Execute a SQL query and return a QueryResult.

        Materialisation is lazy: row tuples / numpy arrays / DataFrames
        are built on first access. The underlying C result is held alive
        by the QueryResult and freed when it is garbage collected.
        """
        result_ptr = ctypes.c_void_p()
        status = self._lib.slothdb_query(
            self._conn, query.encode("utf-8"), ctypes.byref(result_ptr))

        if status != 0:
            error = self._lib.slothdb_result_error(result_ptr)
            error_msg = error.decode("utf-8") if error else "Unknown error"
            self._lib.slothdb_free_result(result_ptr)
            raise RuntimeError(f"Query failed: {error_msg}")

        num_cols = self._lib.slothdb_column_count(result_ptr)
        num_rows = self._lib.slothdb_row_count(result_ptr)

        columns = []
        col_types = []
        for c in range(num_cols):
            name = self._lib.slothdb_column_name(result_ptr, c)
            columns.append({"name": name.decode("utf-8") if name else f"col{c}"})
            col_types.append(
                self._lib.slothdb_column_type(result_ptr, c)
                if self._has_batch_api else 0)

        return QueryResult(self._lib, result_ptr, columns,
                           col_types, num_rows, self._has_batch_api)

    def execute(self, query):
        """Execute a SQL statement (no result expected)."""
        return self.sql(query)

    def close(self):
        """Close the connection."""
        if self._lib and self._conn:
            self._lib.slothdb_disconnect(self._conn)
            self._conn = None
        if self._lib and self._db:
            self._lib.slothdb_close(self._db)
            self._db = None

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def connect(path=""):
    """
    Connect to a SlothDB database.

    Args:
        path: Path to database file. Empty string for in-memory database.

    Returns:
        Connection object.

    Examples:
        db = slothdb.connect()  # in-memory
        db = slothdb.connect("analytics.slothdb")  # persistent
        result = db.sql("SELECT 42")
    """
    return Connection(path)


def sql(query):
    """Shortcut: open an in-memory connection and run one query."""
    return connect().sql(query)


def demo():
    """Run a self-contained 3-query demo with timing (vs DuckDB if installed)."""
    from . import _demo
    _demo.run()

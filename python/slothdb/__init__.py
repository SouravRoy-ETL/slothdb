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

__version__ = "0.1.3"


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
        # Try system path
        try:
            if platform.system() == "Windows":
                return ctypes.CDLL("slothdb.dll")
            else:
                return ctypes.CDLL("libslothdb.so")
        except OSError:
            raise ImportError(
                "Cannot find SlothDB library. Build from source:\n"
                "  cmake -B build -DBUILD_SHARED_LIBS=ON\n"
                "  cmake --build build --config Release"
            )

    return ctypes.CDLL(lib_path)


class QueryResult:
    """Result of a SQL query."""

    def __init__(self, columns, rows):
        self.columns = columns
        self.rows = rows

    @property
    def column_names(self):
        return [c["name"] for c in self.columns]

    @property
    def column_count(self):
        return len(self.columns)

    @property
    def row_count(self):
        return len(self.rows)

    def fetchone(self):
        if self.rows:
            return self.rows[0]
        return None

    def fetchall(self):
        return self.rows

    def fetchdf(self):
        """Convert to pandas DataFrame (requires pandas)."""
        try:
            import pandas as pd
            return pd.DataFrame(self.rows, columns=self.column_names)
        except ImportError:
            raise ImportError("pandas is required for fetchdf()")

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
            lines.append(f"... ({len(self.rows)} rows total)")
        return "\n".join(lines)

    def show(self):
        print(self.__repr__())

    def __len__(self):
        return len(self.rows)


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
        try:
            self._lib = _load_library()
        except ImportError:
            # Fallback: use subprocess with slothdb_shell
            self._lib = None
            return

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
        lib.slothdb_value_varchar.restype = ctypes.c_char_p
        lib.slothdb_value_is_null.restype = ctypes.c_int
        lib.slothdb_value_int32.restype = ctypes.c_int32
        lib.slothdb_value_int64.restype = ctypes.c_int64
        lib.slothdb_value_double.restype = ctypes.c_double
        lib.slothdb_result_error.restype = ctypes.c_char_p
        lib.slothdb_free_result.argtypes = [ctypes.c_void_p]
        lib.slothdb_close.argtypes = [ctypes.c_void_p]
        lib.slothdb_disconnect.argtypes = [ctypes.c_void_p]

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
        """Execute a SQL query and return results."""
        if self._lib is None:
            raise RuntimeError("SlothDB library not loaded")

        result_ptr = ctypes.c_void_p()
        status = self._lib.slothdb_query(
            self._conn, query.encode("utf-8"), ctypes.byref(result_ptr))

        if status != 0:
            error = self._lib.slothdb_result_error(result_ptr)
            error_msg = error.decode("utf-8") if error else "Unknown error"
            self._lib.slothdb_free_result(result_ptr)
            raise RuntimeError(f"Query failed: {error_msg}")

        # Extract results
        num_cols = self._lib.slothdb_column_count(result_ptr)
        num_rows = self._lib.slothdb_row_count(result_ptr)

        columns = []
        for c in range(num_cols):
            name = self._lib.slothdb_column_name(result_ptr, c)
            columns.append({"name": name.decode("utf-8") if name else f"col{c}"})

        rows = []
        for r in range(num_rows):
            row = []
            for c in range(num_cols):
                if self._lib.slothdb_value_is_null(result_ptr, r, c):
                    row.append(None)
                else:
                    val = self._lib.slothdb_value_varchar(result_ptr, r, c)
                    row.append(val.decode("utf-8") if val else "")
            rows.append(tuple(row))

        self._lib.slothdb_free_result(result_ptr)
        return QueryResult(columns, rows)

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

# SlothDB Documentation

Complete guide to using SlothDB — the embedded analytical database engine.

---

**Table of Contents**

- [Getting Started](#getting-started)
  - [Installation](#installation)
  - [CLI Shell](#cli-shell)
  - [Persistent vs In-Memory](#persistent-vs-in-memory)
- [SQL Reference](#sql-reference)
  - [Data Types](#data-types)
  - [DDL — Creating and Modifying Tables](#ddl--creating-and-modifying-tables)
  - [DML — Querying and Modifying Data](#dml--querying-and-modifying-data)
  - [Joins](#joins)
  - [Subqueries](#subqueries)
  - [Common Table Expressions (CTEs)](#common-table-expressions-ctes)
  - [Set Operations](#set-operations)
  - [Window Functions](#window-functions)
  - [QUALIFY](#qualify)
  - [Transactions](#transactions)
  - [EXPLAIN](#explain)
  - [COPY — Import and Export](#copy--import-and-export)
- [Functions](#functions)
  - [Aggregate Functions](#aggregate-functions)
  - [String Functions](#string-functions)
  - [Math Functions](#math-functions)
  - [Trigonometric Functions](#trigonometric-functions)
  - [Date/Time Functions](#datetime-functions)
  - [Null Handling Functions](#null-handling-functions)
  - [Regex Functions](#regex-functions)
  - [Type Casting](#type-casting)
  - [Conditional Expressions](#conditional-expressions)
- [File I/O — Querying External Files](#file-io--querying-external-files)
  - [CSV](#csv)
  - [Parquet](#parquet)
  - [JSON / NDJSON](#json--ndjson)
  - [Excel (XLSX)](#excel-xlsx)
  - [Arrow IPC](#arrow-ipc)
  - [Avro](#avro)
  - [SQLite](#sqlite)
  - [Auto-Detection](#auto-detection)
  - [Glob Patterns](#glob-patterns)
  - [GENERATE_SERIES](#generate_series)
- [Importing Large Datasets](#importing-large-datasets)
- [Python API](#python-api)
  - [Installation](#python-installation)
  - [Connecting](#connecting)
  - [Running Queries](#running-queries)
  - [Query Results](#query-results)
  - [Pandas Integration](#pandas-integration)
  - [Context Manager](#context-manager)
  - [Full Python Example](#full-python-example)
- [C/C++ API](#cc-api)
  - [Including SlothDB](#including-slothdb)
  - [Database Lifecycle](#database-lifecycle)
  - [Executing Queries](#executing-queries)
  - [Reading Results](#reading-results)
  - [Error Handling](#error-handling)
  - [Full C Example](#full-c-example)
  - [Full C++ Example](#full-c-example-1)
  - [Building with CMake](#building-with-cmake)
- [CLI Shell Reference](#cli-shell-reference)
  - [Shell Commands](#shell-commands)
  - [Command-Line Flags](#command-line-flags)
- [GPU Acceleration](#gpu-acceleration)
- [Extensions](#extensions)

---

## Getting Started

### Installation

| Platform | Command |
|----------|---------|
| **Linux / macOS** | `curl -fsSL https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/install.sh \| bash` |
| **Ubuntu / Debian** | `sudo dpkg -i slothdb_0.1.0_amd64.deb` |
| **Fedora / RHEL** | `sudo rpm -i slothdb-0.1.0.rpm` |
| **Arch Linux** | `makepkg -si` using the provided PKGBUILD |
| **macOS (Homebrew)** | `brew install --build-from-source packaging/homebrew/slothdb.rb` |
| **Windows** | Download `slothdb.exe` from [Releases](https://github.com/SouravRoy-ETL/slothdb/releases/latest) |
| **Python** | `pip install slothdb` |

**Build from source:**

```bash
git clone https://github.com/SouravRoy-ETL/slothdb.git
cd slothdb
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/src/slothdb          # Linux/macOS
build\src\Release\slothdb.exe  # Windows
```

### CLI Shell

```bash
slothdb                          # in-memory database
slothdb analytics.slothdb        # persistent database
slothdb -c "SELECT 42"           # run a single query
```

Once inside the shell:

```
slothdb> SELECT 'Hello, World!' AS greeting;
greeting
---------------
Hello, World!
```

### Persistent vs In-Memory

```bash
# In-memory — data is lost when you exit
slothdb

# Persistent — data is saved to a .slothdb file
slothdb mydata.slothdb
```

All `CREATE TABLE` and `INSERT` operations are automatically persisted when using a file-backed database.

---

## SQL Reference

### Data Types

| Type | Aliases | Size | Description |
|------|---------|------|-------------|
| `BOOLEAN` | `BOOL` | 1 byte | `TRUE` / `FALSE` |
| `TINYINT` | `INT1` | 1 byte | -128 to 127 |
| `SMALLINT` | `INT2` | 2 bytes | -32,768 to 32,767 |
| `INTEGER` | `INT`, `INT4` | 4 bytes | -2.1B to 2.1B |
| `BIGINT` | `INT8` | 8 bytes | -9.2e18 to 9.2e18 |
| `HUGEINT` | | 16 bytes | 128-bit signed integer |
| `FLOAT` | `REAL`, `FLOAT4` | 4 bytes | 32-bit floating point |
| `DOUBLE` | `FLOAT8` | 8 bytes | 64-bit floating point |
| `DECIMAL(p,s)` | `NUMERIC` | varies | Fixed-point decimal |
| `VARCHAR` | `TEXT`, `STRING` | varies | Variable-length string |
| `BLOB` | `BYTEA` | varies | Binary data |
| `DATE` | | 4 bytes | Calendar date |
| `TIME` | | 8 bytes | Time of day |
| `TIMESTAMP` | | 8 bytes | Date and time (microsecond precision) |

### DDL — Creating and Modifying Tables

**CREATE TABLE**

```sql
CREATE TABLE employees (
    id INTEGER PRIMARY KEY,
    name VARCHAR NOT NULL,
    department VARCHAR,
    salary DOUBLE,
    hire_date DATE
);

-- Only create if it doesn't already exist
CREATE TABLE IF NOT EXISTS employees (...);

-- Create from a query result
CREATE TABLE top_earners AS
    SELECT * FROM employees WHERE salary > 100000;

-- Create from a file
CREATE TABLE logs AS SELECT * FROM read_csv('server_logs.csv');
```

**DROP TABLE**

```sql
DROP TABLE employees;
DROP TABLE IF EXISTS employees;
```

**ALTER TABLE**

```sql
-- Add a column
ALTER TABLE employees ADD COLUMN email VARCHAR;

-- Remove a column
ALTER TABLE employees DROP COLUMN email;

-- Rename a column
ALTER TABLE employees RENAME COLUMN dept TO department;
```

**TRUNCATE**

```sql
-- Remove all rows, keep the table structure
TRUNCATE TABLE employees;
```

**Views**

```sql
-- Create a view
CREATE VIEW active_employees AS
    SELECT * FROM employees WHERE status = 'active';

-- Replace an existing view
CREATE OR REPLACE VIEW active_employees AS
    SELECT * FROM employees WHERE end_date IS NULL;

-- Drop a view
DROP VIEW active_employees;
DROP VIEW IF EXISTS active_employees;
```

### DML — Querying and Modifying Data

**SELECT**

```sql
-- Basic select
SELECT * FROM employees;

-- With columns and aliases
SELECT name, salary * 12 AS annual_salary FROM employees;

-- Filtering
SELECT * FROM employees WHERE department = 'Engineering' AND salary > 80000;

-- Sorting
SELECT * FROM employees ORDER BY salary DESC NULLS LAST;

-- Pagination
SELECT * FROM employees ORDER BY id LIMIT 20 OFFSET 40;

-- Distinct
SELECT DISTINCT department FROM employees;
```

**INSERT**

```sql
-- Single row
INSERT INTO employees VALUES (1, 'Alice', 'Engineering', 95000, '2022-01-15');

-- Multiple rows
INSERT INTO employees VALUES
    (2, 'Bob', 'Sales', 72000, '2021-06-01'),
    (3, 'Charlie', 'Engineering', 110000, '2020-03-22');

-- Specific columns
INSERT INTO employees (id, name, department) VALUES (4, 'Diana', 'Marketing');

-- Insert from a query
INSERT INTO archive SELECT * FROM employees WHERE hire_date < '2020-01-01';
```

**UPDATE**

```sql
UPDATE employees SET salary = salary * 1.10 WHERE department = 'Engineering';
UPDATE employees SET department = 'Product', salary = 95000 WHERE id = 4;
```

**DELETE**

```sql
DELETE FROM employees WHERE id = 3;
DELETE FROM employees WHERE hire_date < '2019-01-01';
DELETE FROM employees;  -- delete all rows
```

**MERGE** (upsert)

```sql
MERGE INTO employees AS target
USING new_data AS source
ON target.id = source.id
WHEN MATCHED THEN
    UPDATE SET salary = source.salary, department = source.department
WHEN NOT MATCHED THEN
    INSERT (id, name, department, salary) VALUES (source.id, source.name, source.department, source.salary);
```

### Joins

```sql
-- Inner join
SELECT e.name, d.department_name
FROM employees e
INNER JOIN departments d ON e.dept_id = d.id;

-- Left join — all employees, even without a department
SELECT e.name, d.department_name
FROM employees e
LEFT JOIN departments d ON e.dept_id = d.id;

-- Right join
SELECT e.name, d.department_name
FROM employees e
RIGHT JOIN departments d ON e.dept_id = d.id;

-- Full outer join
SELECT e.name, d.department_name
FROM employees e
FULL OUTER JOIN departments d ON e.dept_id = d.id;

-- Cross join — cartesian product
SELECT * FROM colors CROSS JOIN sizes;

-- Natural join — joins on columns with matching names
SELECT * FROM orders NATURAL JOIN customers;

-- Join using — shorthand when column names match
SELECT * FROM orders JOIN customers USING (customer_id);

-- Self join
SELECT a.name AS employee, b.name AS manager
FROM employees a
JOIN employees b ON a.manager_id = b.id;

-- Multiple joins
SELECT o.id, c.name, p.product_name
FROM orders o
JOIN customers c ON o.customer_id = c.id
JOIN products p ON o.product_id = p.id;
```

### Subqueries

```sql
-- Subquery in WHERE
SELECT * FROM employees
WHERE salary > (SELECT AVG(salary) FROM employees);

-- Subquery in FROM
SELECT dept, avg_salary
FROM (SELECT department AS dept, AVG(salary) AS avg_salary
      FROM employees GROUP BY department) sub
WHERE avg_salary > 80000;

-- EXISTS
SELECT * FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id);

-- NOT EXISTS
SELECT * FROM customers c
WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id);

-- IN with subquery
SELECT * FROM employees
WHERE department IN (SELECT name FROM departments WHERE budget > 1000000);
```

### Common Table Expressions (CTEs)

```sql
-- Basic CTE
WITH engineering AS (
    SELECT * FROM employees WHERE department = 'Engineering'
)
SELECT name, salary FROM engineering ORDER BY salary DESC;

-- Multiple CTEs
WITH
    dept_stats AS (
        SELECT department, AVG(salary) AS avg_sal, COUNT(*) AS cnt
        FROM employees GROUP BY department
    ),
    top_depts AS (
        SELECT * FROM dept_stats WHERE avg_sal > 90000
    )
SELECT * FROM top_depts ORDER BY avg_sal DESC;

-- Recursive CTE (e.g., org chart traversal)
WITH RECURSIVE org_chart(id, name, manager_id, level) AS (
    -- Base case: CEO (no manager)
    SELECT id, name, manager_id, 0 AS level
    FROM employees WHERE manager_id IS NULL

    UNION ALL

    -- Recursive case: employees under each manager
    SELECT e.id, e.name, e.manager_id, oc.level + 1
    FROM employees e
    JOIN org_chart oc ON e.manager_id = oc.id
)
SELECT * FROM org_chart ORDER BY level, name;

-- Recursive CTE: generate a number sequence
WITH RECURSIVE nums(n) AS (
    SELECT 1
    UNION ALL
    SELECT n + 1 FROM nums WHERE n < 100
)
SELECT n FROM nums;
```

### Set Operations

```sql
-- UNION — combine results, remove duplicates
SELECT name FROM employees_us
UNION
SELECT name FROM employees_eu;

-- UNION ALL — combine results, keep duplicates (faster)
SELECT name FROM employees_us
UNION ALL
SELECT name FROM employees_eu;

-- INTERSECT — rows that appear in both
SELECT customer_id FROM orders_2024
INTERSECT
SELECT customer_id FROM orders_2025;

-- EXCEPT — rows in first but not in second
SELECT customer_id FROM newsletter_subscribers
EXCEPT
SELECT customer_id FROM unsubscribed;
```

### Window Functions

Window functions compute values across a set of rows related to the current row without collapsing them.

```sql
-- ROW_NUMBER — unique sequential number per partition
SELECT name, department, salary,
    ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) AS rank
FROM employees;

-- RANK — same salary gets same rank, with gaps
SELECT name, salary,
    RANK() OVER (ORDER BY salary DESC) AS rank
FROM employees;

-- DENSE_RANK — same salary gets same rank, no gaps
SELECT name, salary,
    DENSE_RANK() OVER (ORDER BY salary DESC) AS dense_rank
FROM employees;

-- NTILE — divide rows into N equal buckets
SELECT name, salary,
    NTILE(4) OVER (ORDER BY salary DESC) AS quartile
FROM employees;

-- LAG / LEAD — access previous/next row's value
SELECT date, revenue,
    LAG(revenue, 1) OVER (ORDER BY date) AS prev_day_revenue,
    LEAD(revenue, 1) OVER (ORDER BY date) AS next_day_revenue,
    revenue - LAG(revenue, 1) OVER (ORDER BY date) AS daily_change
FROM daily_sales;

-- FIRST_VALUE / LAST_VALUE
SELECT name, department, salary,
    FIRST_VALUE(name) OVER (PARTITION BY department ORDER BY salary DESC) AS top_earner
FROM employees;

-- Running totals with SUM OVER
SELECT date, amount,
    SUM(amount) OVER (ORDER BY date) AS running_total
FROM transactions;

-- Moving average
SELECT date, revenue,
    AVG(revenue) OVER (ORDER BY date) AS cumulative_avg
FROM daily_sales;
```

### QUALIFY

Filter rows based on window function results — no subquery needed. Inspired by Snowflake.

```sql
-- Get the highest-paid employee per department
SELECT name, department, salary
FROM employees
QUALIFY ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) = 1;

-- Get top 3 products per category by revenue
SELECT product_name, category, revenue
FROM products
QUALIFY RANK() OVER (PARTITION BY category ORDER BY revenue DESC) <= 3;

-- Without QUALIFY, you'd need a subquery:
SELECT * FROM (
    SELECT name, department, salary,
        ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) AS rn
    FROM employees
) sub WHERE rn = 1;
```

### Transactions

```sql
BEGIN TRANSACTION;
INSERT INTO accounts VALUES (1, 'Alice', 5000);
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
COMMIT;

-- Roll back on error
BEGIN;
DELETE FROM important_data;
ROLLBACK;  -- undo the delete
```

### EXPLAIN

See the query execution plan:

```sql
EXPLAIN SELECT department, AVG(salary)
FROM employees
WHERE hire_date > '2020-01-01'
GROUP BY department;

-- Output shows: AGGREGATE → FILTER → SCAN employees
```

### COPY — Import and Export

```sql
-- Export to CSV
COPY employees TO 'employees_backup.csv';

-- Export to Parquet (compressed, columnar)
COPY employees TO 'employees.parquet' WITH (FORMAT PARQUET);

-- Export to JSON
COPY employees TO 'employees.json' WITH (FORMAT JSON);

-- Export query results
COPY (SELECT * FROM employees WHERE salary > 100000) TO 'top_earners.csv';

-- Import from CSV
COPY employees FROM 'new_hires.csv';

-- Import from JSON
COPY employees FROM 'data.json' WITH (FORMAT JSON);

-- CSV options
COPY employees TO 'data.tsv' WITH (DELIMITER '\t', HEADER TRUE);
```

---

## Functions

### Aggregate Functions

| Function | Description | Example |
|----------|-------------|---------|
| `COUNT(*)` | Count all rows | `SELECT COUNT(*) FROM t` |
| `COUNT(col)` | Count non-NULL values | `SELECT COUNT(name) FROM t` |
| `COUNT(DISTINCT col)` | Count unique values | `SELECT COUNT(DISTINCT dept) FROM t` |
| `SUM(col)` | Sum of values | `SELECT SUM(salary) FROM t` |
| `AVG(col)` | Average | `SELECT AVG(salary) FROM t` |
| `MIN(col)` | Minimum value | `SELECT MIN(hire_date) FROM t` |
| `MAX(col)` | Maximum value | `SELECT MAX(salary) FROM t` |
| `STRING_AGG(col, sep)` | Concatenate strings | `SELECT STRING_AGG(name, ', ') FROM t` |
| `STDDEV(col)` | Sample standard deviation | `SELECT STDDEV(salary) FROM t` |
| `STDDEV_POP(col)` | Population standard deviation | `SELECT STDDEV_POP(salary) FROM t` |
| `VARIANCE(col)` | Sample variance | `SELECT VARIANCE(salary) FROM t` |
| `VAR_POP(col)` | Population variance | `SELECT VAR_POP(salary) FROM t` |
| `MEDIAN(col)` | Median value | `SELECT MEDIAN(salary) FROM t` |
| `BOOL_AND(col)` | Logical AND of all values | `SELECT BOOL_AND(is_active) FROM t` |
| `BOOL_OR(col)` | Logical OR of all values | `SELECT BOOL_OR(is_active) FROM t` |

Aliases: `GROUP_CONCAT` and `LISTAGG` work as aliases for `STRING_AGG`. `VAR_SAMP` works as an alias for `VARIANCE`. `STDDEV_SAMP` works as an alias for `STDDEV`.

### String Functions

| Function | Description | Example |
|----------|-------------|---------|
| `LENGTH(s)` | String length | `LENGTH('hello')` → `5` |
| `UPPER(s)` | To uppercase | `UPPER('hello')` → `'HELLO'` |
| `LOWER(s)` | To lowercase | `LOWER('HELLO')` → `'hello'` |
| `CONCAT(s1, s2, ...)` | Concatenate strings | `CONCAT('a', 'b', 'c')` → `'abc'` |
| `s1 \|\| s2` | Concatenation operator | `'hello' \|\| ' world'` → `'hello world'` |
| `SUBSTRING(s, start, len)` | Extract substring | `SUBSTRING('hello', 2, 3)` → `'ell'` |
| `REPLACE(s, from, to)` | Replace occurrences | `REPLACE('foo', 'o', 'a')` → `'faa'` |
| `TRIM(s)` | Remove leading/trailing spaces | `TRIM('  hi  ')` → `'hi'` |
| `LTRIM(s)` | Remove leading spaces | `LTRIM('  hi')` → `'hi'` |
| `RTRIM(s)` | Remove trailing spaces | `RTRIM('hi  ')` → `'hi'` |
| `LEFT(s, n)` | First n characters | `LEFT('hello', 3)` → `'hel'` |
| `RIGHT(s, n)` | Last n characters | `RIGHT('hello', 3)` → `'llo'` |
| `LPAD(s, len, pad)` | Pad left to length | `LPAD('42', 5, '0')` → `'00042'` |
| `RPAD(s, len, pad)` | Pad right to length | `RPAD('hi', 5, '.')` → `'hi...'` |
| `REVERSE(s)` | Reverse string | `REVERSE('hello')` → `'olleh'` |
| `REPEAT(s, n)` | Repeat string n times | `REPEAT('ha', 3)` → `'hahaha'` |
| `POSITION(sub IN s)` | Find substring position | `POSITION('ll' IN 'hello')` → `3` |
| `STARTS_WITH(s, prefix)` | Check prefix | `STARTS_WITH('hello', 'he')` → `true` |
| `ENDS_WITH(s, suffix)` | Check suffix | `ENDS_WITH('hello', 'lo')` → `true` |
| `CONTAINS(s, sub)` | Check if contains | `CONTAINS('hello', 'ell')` → `true` |
| `SPLIT_PART(s, delim, idx)` | Split and get part | `SPLIT_PART('a-b-c', '-', 2)` → `'b'` |
| `INITCAP(s)` | Capitalize each word | `INITCAP('hello world')` → `'Hello World'` |

Aliases: `CHAR_LENGTH` works as an alias for `LENGTH`. `SUBSTR` works as an alias for `SUBSTRING`. `STRPOS(s, sub)` works as an alias for `POSITION`. `PREFIX` and `SUFFIX` work as aliases for `STARTS_WITH` and `ENDS_WITH`.

### Math Functions

| Function | Description | Example |
|----------|-------------|---------|
| `ABS(x)` | Absolute value | `ABS(-5)` → `5` |
| `CEIL(x)` | Round up | `CEIL(3.2)` → `4` |
| `FLOOR(x)` | Round down | `FLOOR(3.8)` → `3` |
| `ROUND(x)` | Round to nearest | `ROUND(3.5)` → `4` |
| `SQRT(x)` | Square root | `SQRT(16)` → `4` |
| `POWER(x, y)` | Exponentiation | `POWER(2, 10)` → `1024` |
| `MOD(x, y)` | Modulo (remainder) | `MOD(10, 3)` → `1` |
| `LOG(x)` | Natural logarithm | `LOG(2.718)` → `~1.0` |
| `LOG2(x)` | Base-2 logarithm | `LOG2(8)` → `3` |
| `LOG10(x)` | Base-10 logarithm | `LOG10(100)` → `2` |
| `EXP(x)` | e raised to x | `EXP(1)` → `2.718...` |
| `SIGN(x)` | Sign (-1, 0, or 1) | `SIGN(-42)` → `-1` |
| `TRUNC(x)` | Truncate to integer | `TRUNC(3.9)` → `3` |
| `PI()` | Pi constant | `PI()` → `3.14159...` |
| `RANDOM()` | Random 0.0 to 1.0 | `RANDOM()` → `0.7231...` |
| `LEAST(a, b, ...)` | Minimum of values | `LEAST(5, 3, 9)` → `3` |
| `GREATEST(a, b, ...)` | Maximum of values | `GREATEST(5, 3, 9)` → `9` |

Aliases: `CEILING` works as an alias for `CEIL`. `LN` works as an alias for `LOG`. `TRUNCATE` works as an alias for `TRUNC`. `RAND` works as an alias for `RANDOM`.

### Trigonometric Functions

| Function | Description |
|----------|-------------|
| `SIN(x)` | Sine (radians) |
| `COS(x)` | Cosine (radians) |
| `TAN(x)` | Tangent (radians) |
| `ASIN(x)` | Arcsine |
| `ACOS(x)` | Arccosine |
| `ATAN(x)` | Arctangent |
| `ATAN2(y, x)` | Two-argument arctangent |
| `DEGREES(x)` | Radians to degrees |
| `RADIANS(x)` | Degrees to radians |

### Date/Time Functions

| Function | Description | Example |
|----------|-------------|---------|
| `NOW()` | Current timestamp | `NOW()` |
| `CURRENT_TIMESTAMP` | Current timestamp | `SELECT CURRENT_TIMESTAMP` |
| `CURRENT_DATE` | Current date | `SELECT CURRENT_DATE` |
| `EXTRACT(part FROM ts)` | Extract component | `EXTRACT(YEAR FROM ts)` |
| `DATE_PART(part, ts)` | Extract component (alias) | `DATE_PART('month', ts)` |
| `DATE_ADD(part, n, ts)` | Add interval | `DATE_ADD('day', 7, ts)` |
| `DATE_DIFF(part, ts1, ts2)` | Difference between dates | `DATE_DIFF('day', start, end)` |
| `DATE_TRUNC(part, ts)` | Truncate to unit | `DATE_TRUNC('month', ts)` |
| `STRFTIME(fmt, ts)` | Format timestamp | `STRFTIME('%Y-%m-%d', ts)` |
| `TO_TIMESTAMP(epoch)` | Epoch seconds to timestamp | `TO_TIMESTAMP(1700000000)` |
| `EPOCH_MS(ts)` | Timestamp to epoch ms | `EPOCH_MS(ts)` |

**EXTRACT parts:** `YEAR`, `MONTH`, `DAY`, `HOUR`, `MINUTE`, `SECOND`, `EPOCH`, `DOW` (day of week)

**DATE_DIFF / DATE_ADD parts:** `SECOND`, `MINUTE`, `HOUR`, `DAY`

Aliases: `DATEADD` works as an alias for `DATE_ADD`. `DATEDIFF` works as an alias for `DATE_DIFF`. `FORMAT_TIMESTAMP` works as an alias for `STRFTIME`. `MAKE_TIMESTAMP` works as an alias for `TO_TIMESTAMP`.

### Null Handling Functions

| Function | Description | Example |
|----------|-------------|---------|
| `COALESCE(a, b, ...)` | First non-NULL value | `COALESCE(phone, email, 'N/A')` |
| `NULLIF(a, b)` | NULL if a = b, else a | `NULLIF(score, 0)` — avoid division by zero |

### Regex Functions

| Function | Description | Example |
|----------|-------------|---------|
| `REGEXP_MATCHES(s, pattern)` | Test if pattern matches | `REGEXP_MATCHES(email, '.*@.*\.com')` |
| `REGEXP_REPLACE(s, pattern, repl)` | Replace matches | `REGEXP_REPLACE(phone, '[^0-9]', '')` |
| `REGEXP_EXTRACT(s, pattern)` | Extract first match | `REGEXP_EXTRACT(url, 'https?://([^/]+)')` |

Alias: `REGEXP_MATCH` works as an alias for `REGEXP_MATCHES`.

### Type Casting

```sql
-- CAST — error on invalid conversion
SELECT CAST('42' AS INTEGER);
SELECT CAST(3.14 AS VARCHAR);
SELECT CAST('2024-01-15' AS DATE);

-- TRY_CAST — returns NULL instead of error
SELECT TRY_CAST('not_a_number' AS INTEGER);  -- returns NULL
SELECT TRY_CAST('42' AS INTEGER);            -- returns 42
```

### Conditional Expressions

```sql
-- CASE WHEN
SELECT name,
    CASE
        WHEN salary > 100000 THEN 'Senior'
        WHEN salary > 60000  THEN 'Mid'
        ELSE 'Junior'
    END AS level
FROM employees;

-- CASE with value
SELECT name,
    CASE department
        WHEN 'Engineering' THEN 'ENG'
        WHEN 'Sales'       THEN 'SLS'
        ELSE 'OTHER'
    END AS dept_code
FROM employees;

-- BETWEEN
SELECT * FROM events WHERE event_date BETWEEN '2024-01-01' AND '2024-12-31';

-- IN
SELECT * FROM employees WHERE department IN ('Engineering', 'Product', 'Design');

-- LIKE pattern matching
SELECT * FROM employees WHERE name LIKE 'A%';       -- starts with A
SELECT * FROM employees WHERE email LIKE '%@gmail%'; -- contains @gmail

-- ILIKE (case-insensitive LIKE)
SELECT * FROM employees WHERE name ILIKE 'alice';

-- IS NULL / IS NOT NULL
SELECT * FROM employees WHERE manager_id IS NULL;
SELECT * FROM employees WHERE phone IS NOT NULL;
```

---

## File I/O — Querying External Files

SlothDB can query files directly without importing them into tables. All file formats are built-in — no extensions needed.

### CSV

```sql
-- Read a CSV file
SELECT * FROM read_csv('sales_data.csv');

-- Query with filtering (only matching rows are loaded)
SELECT product, SUM(revenue)
FROM read_csv('sales_data.csv')
WHERE region = 'US'
GROUP BY product;

-- Import into a table
CREATE TABLE sales AS SELECT * FROM read_csv('sales_data.csv');

-- Auto-detect from file extension
SELECT * FROM 'sales_data.csv';
```

### Parquet

Best format for large datasets — columnar, compressed, with statistics for scan skipping.

```sql
-- Read Parquet
SELECT * FROM read_parquet('events.parquet');

-- Parquet pushes filters down to row groups for fast scans
SELECT * FROM read_parquet('events.parquet') WHERE event_date > '2024-01-01';

-- Convert CSV to Parquet for faster future queries
COPY (SELECT * FROM read_csv('huge.csv')) TO 'huge.parquet' WITH (FORMAT PARQUET);
```

### JSON / NDJSON

Supports both JSON arrays (`[{...}, {...}]`) and newline-delimited JSON (one object per line).

```sql
-- Read JSON
SELECT * FROM read_json('users.json');

-- Read NDJSON (newline-delimited)
SELECT * FROM read_json('events.ndjson');

-- Auto-detect
SELECT * FROM 'events.json';
```

### Excel (XLSX)

```sql
-- Read Excel file (first sheet)
SELECT * FROM read_xlsx('report.xlsx');

-- Auto-detect
SELECT * FROM 'report.xlsx';
```

### Arrow IPC

```sql
-- Read Arrow IPC (Feather) file
SELECT * FROM read_arrow('data.arrow');
SELECT * FROM read_arrow('data.feather');
```

### Avro

```sql
-- Read Avro file
SELECT * FROM read_avro('events.avro');
```

### SQLite

Read tables directly from SQLite database files — no libsqlite3 dependency needed.

```sql
-- Scan a table from a SQLite database
SELECT * FROM sqlite_scan('legacy_app.db', 'users');

-- Join SlothDB tables with SQLite data
SELECT e.name, s.old_score
FROM employees e
JOIN sqlite_scan('old_system.db', 'scores') s ON e.id = s.employee_id;
```

### Auto-Detection

When you query a string literal in the `FROM` clause, SlothDB auto-detects the format by file extension:

```sql
SELECT * FROM 'data.csv';        -- detected as CSV
SELECT * FROM 'data.parquet';    -- detected as Parquet
SELECT * FROM 'data.json';      -- detected as JSON
SELECT * FROM 'report.xlsx';    -- detected as Excel
SELECT * FROM 'data.arrow';     -- detected as Arrow IPC
SELECT * FROM 'data.avro';      -- detected as Avro
```

### Glob Patterns

Query multiple files at once:

```sql
-- All CSV files in a directory
SELECT * FROM read_csv('logs/*.csv');

-- All Parquet files
SELECT * FROM read_parquet('data/year=2024/*.parquet');
```

### GENERATE_SERIES

Generate a sequence of numbers:

```sql
-- Numbers 1 to 10
SELECT * FROM GENERATE_SERIES(1, 10);

-- Even numbers 0 to 100
SELECT * FROM GENERATE_SERIES(0, 100, 2);

-- Use in queries
SELECT gs.generate_series AS n, n * n AS square
FROM GENERATE_SERIES(1, 10) gs;
```

---

## Importing Large Datasets

For large datasets, here are the recommended approaches:

**1. Query directly without importing (recommended for one-off analysis):**

```sql
-- SlothDB streams through the file — doesn't load everything into memory
SELECT region, SUM(revenue) FROM read_csv('huge_file.csv') GROUP BY region;
```

**2. Import into a persistent database (recommended for repeated queries):**

```bash
slothdb analytics.slothdb
```

```sql
-- Load from CSV
CREATE TABLE sales AS SELECT * FROM read_csv('sales_2024.csv');

-- Load from Parquet (fastest)
CREATE TABLE events AS SELECT * FROM read_parquet('events.parquet');

-- Load only what you need
CREATE TABLE recent_sales AS
    SELECT * FROM read_csv('all_sales.csv') WHERE year >= 2023;
```

**3. Convert to Parquet first (recommended for CSVs you'll query repeatedly):**

```sql
-- Convert CSV to Parquet (one-time cost)
COPY (SELECT * FROM read_csv('huge.csv')) TO 'huge.parquet' WITH (FORMAT PARQUET);

-- Now all future queries are much faster
SELECT category, COUNT(*) FROM read_parquet('huge.parquet') GROUP BY category;
```

**Why Parquet is best for large data:**
- Columnar — only reads columns in your SELECT
- Compressed — 5-10x smaller than CSV
- Row group statistics — SlothDB skips row groups that don't match your WHERE clause
- No schema detection overhead — types are stored in the file

---

## Python API

### Python Installation

```bash
pip install slothdb
```

Or build from source:

```bash
cd slothdb
cmake -B build -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cd python && pip install .
```

### Connecting

```python
import slothdb

# In-memory database
db = slothdb.connect()

# Persistent database (saved to file)
db = slothdb.connect("analytics.slothdb")
```

### Running Queries

```python
# Execute SQL and get results
result = db.sql("SELECT * FROM read_csv('data.csv')")

# DDL and DML statements
db.execute("CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER)")
db.execute("INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25)")

# Query tables
result = db.sql("SELECT * FROM users WHERE age > 20")
print(result)
```

### Query Results

```python
result = db.sql("SELECT name, age FROM users")

# Access metadata
print(result.column_names)   # ['name', 'age']
print(result.column_count)   # 2
print(result.row_count)      # number of rows
print(len(result))           # same as row_count

# Fetch rows
row = result.fetchone()      # first row as tuple: ('Alice', 30)
rows = result.fetchall()     # all rows as list of tuples

# Pretty print
print(result)
# name       | age
# -----------+-----------
# Alice      | 30
# Bob        | 25
```

### Pandas Integration

```python
import slothdb

db = slothdb.connect()
db.execute("CREATE TABLE t AS SELECT * FROM read_csv('sales.csv')")

result = db.sql("SELECT region, SUM(revenue) AS total FROM t GROUP BY region")
df = result.fetchdf()  # returns a pandas DataFrame

print(df)
#       region    total
# 0    US-East  1250000
# 1    US-West   980000
# 2     Europe   730000
```

### Context Manager

```python
# Automatically closes connection when done
with slothdb.connect("analytics.slothdb") as db:
    db.execute("INSERT INTO logs VALUES (1, 'event', NOW())")
    result = db.sql("SELECT COUNT(*) FROM logs")
    print(result.fetchone())
```

### Full Python Example

```python
import slothdb

# Connect to persistent database
db = slothdb.connect("company.slothdb")

# Create table and load data
db.execute("""
    CREATE TABLE IF NOT EXISTS employees AS
    SELECT * FROM read_csv('employees.csv')
""")

# Analytical query with window functions
result = db.sql("""
    SELECT
        name,
        department,
        salary,
        AVG(salary) OVER (PARTITION BY department) AS dept_avg,
        RANK() OVER (PARTITION BY department ORDER BY salary DESC) AS dept_rank
    FROM employees
    ORDER BY department, dept_rank
""")

# Export to pandas
df = result.fetchdf()
print(df.head(20))

# Export results to Parquet
db.execute("""
    COPY (SELECT * FROM employees WHERE hire_date >= '2023-01-01')
    TO 'recent_hires.parquet' WITH (FORMAT PARQUET)
""")

db.close()
```

---

## C/C++ API

### Including SlothDB

```c
#include "slothdb/api/slothdb.h"
```

Link against the `slothdb` library when building.

### Database Lifecycle

```c
slothdb_database *db;
slothdb_connection *conn;

// Open database (empty string or NULL for in-memory)
slothdb_open("analytics.slothdb", &db);  // persistent
slothdb_open("", &db);                   // in-memory
slothdb_open(NULL, &db);                 // in-memory

// Create a connection
slothdb_connect(db, &conn);

// ... use the connection ...

// Clean up
slothdb_disconnect(conn);
slothdb_close(db);
```

### Executing Queries

```c
slothdb_result *result;
slothdb_status status;

status = slothdb_query(conn, "SELECT 42 AS answer", &result);
if (status != SLOTHDB_OK) {
    const char *error = slothdb_result_error(result);
    fprintf(stderr, "Query failed: %s\n", error);
    slothdb_free_result(result);
    return;
}

// ... read results ...

slothdb_free_result(result);
```

### Reading Results

```c
slothdb_result *result;
slothdb_query(conn, "SELECT name, salary FROM employees", &result);

uint64_t num_cols = slothdb_column_count(result);
uint64_t num_rows = slothdb_row_count(result);

// Print column headers
for (uint64_t c = 0; c < num_cols; c++) {
    printf("%s\t", slothdb_column_name(result, c));
}
printf("\n");

// Print rows
for (uint64_t r = 0; r < num_rows; r++) {
    for (uint64_t c = 0; c < num_cols; c++) {
        if (slothdb_value_is_null(result, r, c)) {
            printf("NULL\t");
        } else {
            printf("%s\t", slothdb_value_varchar(result, r, c));
        }
    }
    printf("\n");
}

slothdb_free_result(result);
```

**Value accessors by type:**

| Function | Returns | Use for |
|----------|---------|---------|
| `slothdb_value_int32(result, row, col)` | `int32_t` | INTEGER columns |
| `slothdb_value_int64(result, row, col)` | `int64_t` | BIGINT columns |
| `slothdb_value_double(result, row, col)` | `double` | FLOAT/DOUBLE columns |
| `slothdb_value_varchar(result, row, col)` | `const char*` | Any column (string representation) |
| `slothdb_value_is_null(result, row, col)` | `int` | Check for NULL (1 = NULL) |

**Other result functions:**

| Function | Returns | Description |
|----------|---------|-------------|
| `slothdb_column_count(result)` | `uint64_t` | Number of columns |
| `slothdb_row_count(result)` | `uint64_t` | Number of rows |
| `slothdb_column_name(result, col)` | `const char*` | Column name |
| `slothdb_column_type(result, col)` | `slothdb_type` | Column type enum |
| `slothdb_result_error(result)` | `const char*` | Error message (if query failed) |
| `slothdb_version()` | `const char*` | SlothDB version string |

**Type enum values:**

| Constant | Value | Description |
|----------|-------|-------------|
| `SLOTHDB_TYPE_BOOLEAN` | 2 | Boolean |
| `SLOTHDB_TYPE_INTEGER` | 5 | 32-bit integer |
| `SLOTHDB_TYPE_BIGINT` | 6 | 64-bit integer |
| `SLOTHDB_TYPE_FLOAT` | 11 | 32-bit float |
| `SLOTHDB_TYPE_DOUBLE` | 12 | 64-bit float |
| `SLOTHDB_TYPE_VARCHAR` | 15 | String |

### Error Handling

```c
slothdb_result *result;
slothdb_status status = slothdb_query(conn, sql, &result);

switch (status) {
    case SLOTHDB_OK:
        // Success — read results
        break;
    case SLOTHDB_ERROR:
        fprintf(stderr, "Error: %s\n", slothdb_result_error(result));
        break;
    case SLOTHDB_INVALID:
        fprintf(stderr, "Invalid argument\n");
        break;
}

// Always free the result, even on error
slothdb_free_result(result);
```

### Full C Example

```c
#include <stdio.h>
#include "slothdb/api/slothdb.h"

int main() {
    slothdb_database *db;
    slothdb_connection *conn;
    slothdb_result *result;

    // Open persistent database
    slothdb_open("company.slothdb", &db);
    slothdb_connect(db, &conn);

    // Create table
    slothdb_query(conn, "CREATE TABLE IF NOT EXISTS employees ("
                        "  id INTEGER PRIMARY KEY,"
                        "  name VARCHAR NOT NULL,"
                        "  salary DOUBLE"
                        ")", &result);
    slothdb_free_result(result);

    // Insert data
    slothdb_query(conn, "INSERT INTO employees VALUES "
                        "(1, 'Alice', 95000),"
                        "(2, 'Bob', 87000),"
                        "(3, 'Charlie', 110000)", &result);
    slothdb_free_result(result);

    // Query
    slothdb_status status = slothdb_query(conn,
        "SELECT name, salary, RANK() OVER (ORDER BY salary DESC) AS rank "
        "FROM employees", &result);

    if (status == SLOTHDB_OK) {
        for (uint64_t r = 0; r < slothdb_row_count(result); r++) {
            printf("%s earns $%.0f (rank %s)\n",
                slothdb_value_varchar(result, r, 0),
                slothdb_value_double(result, r, 1),
                slothdb_value_varchar(result, r, 2));
        }
    }

    slothdb_free_result(result);
    slothdb_disconnect(conn);
    slothdb_close(db);
    return 0;
}
```

### Full C++ Example

```cpp
#include <iostream>
#include <string>
#include "slothdb/api/slothdb.h"

// RAII wrapper for SlothDB
class SlothDB {
    slothdb_database *db_ = nullptr;
    slothdb_connection *conn_ = nullptr;
public:
    SlothDB(const char *path = "") {
        slothdb_open(path, &db_);
        slothdb_connect(db_, &conn_);
    }
    ~SlothDB() {
        if (conn_) slothdb_disconnect(conn_);
        if (db_) slothdb_close(db_);
    }

    void execute(const char *sql) {
        slothdb_result *result;
        slothdb_status s = slothdb_query(conn_, sql, &result);
        if (s != SLOTHDB_OK) {
            std::string err = slothdb_result_error(result);
            slothdb_free_result(result);
            throw std::runtime_error(err);
        }
        slothdb_free_result(result);
    }

    void query(const char *sql) {
        slothdb_result *result;
        slothdb_status s = slothdb_query(conn_, sql, &result);
        if (s != SLOTHDB_OK) {
            std::string err = slothdb_result_error(result);
            slothdb_free_result(result);
            throw std::runtime_error(err);
        }

        uint64_t cols = slothdb_column_count(result);
        uint64_t rows = slothdb_row_count(result);

        for (uint64_t c = 0; c < cols; c++)
            std::cout << slothdb_column_name(result, c) << "\t";
        std::cout << "\n";

        for (uint64_t r = 0; r < rows; r++) {
            for (uint64_t c = 0; c < cols; c++) {
                if (slothdb_value_is_null(result, r, c))
                    std::cout << "NULL\t";
                else
                    std::cout << slothdb_value_varchar(result, r, c) << "\t";
            }
            std::cout << "\n";
        }
        slothdb_free_result(result);
    }
};

int main() {
    SlothDB db("analytics.slothdb");

    db.execute("CREATE TABLE events AS SELECT * FROM read_csv('events.csv')");
    db.query("SELECT event_type, COUNT(*) FROM events GROUP BY event_type ORDER BY COUNT(*) DESC LIMIT 10");

    return 0;
}
```

### Building with CMake

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_app)

# Option A: SlothDB as a subdirectory
add_subdirectory(slothdb)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE slothdb_lib)

# Option B: Find installed SlothDB
find_library(SLOTHDB_LIB slothdb)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE ${SLOTHDB_LIB})
```

Compile manually:

```bash
# Linux / macOS
g++ -std=c++20 -o my_app main.cpp -lslothdb -L/usr/local/lib -I/usr/local/include

# Windows (MSVC)
cl /std:c++20 main.cpp /I path\to\include slothdb.lib
```

---

## CLI Shell Reference

### Shell Commands

| Command | Description |
|---------|-------------|
| `.help` | Show available commands |
| `.quit` or `.exit` | Exit the shell |
| `.tables` | List all tables in the database |
| `.schema` | Show table schemas |
| `.version` | Show SlothDB version |

### Command-Line Flags

```bash
slothdb                            # in-memory database, interactive shell
slothdb mydata.slothdb             # persistent database
slothdb -c "SELECT 42 AS answer"   # run a query and exit
```

**Examples:**

```bash
# Quick one-liner query on a CSV
slothdb -c "SELECT COUNT(*) FROM 'sales.csv'"

# Interactive exploration of a Parquet file
slothdb
slothdb> SELECT * FROM read_parquet('events.parquet') LIMIT 10;
slothdb> SELECT event_type, COUNT(*) FROM read_parquet('events.parquet') GROUP BY event_type;
slothdb> .quit

# Persistent database workflow
slothdb analytics.slothdb
slothdb> CREATE TABLE sales AS SELECT * FROM read_csv('sales.csv');
slothdb> SELECT region, SUM(revenue) FROM sales GROUP BY region;
slothdb> .quit

# Next time, your data is still there
slothdb analytics.slothdb
slothdb> SELECT COUNT(*) FROM sales;
```

---

## GPU Acceleration

SlothDB automatically offloads heavy operations to the GPU when the dataset exceeds 100,000 rows.

**Supported backends:**
- **CUDA** — NVIDIA GPUs
- **Metal** — Apple Silicon (M1/M2/M3/M4)
- **CPU fallback** — automatic when no GPU is available

**GPU-accelerated operations:**
- Aggregation (GROUP BY, SUM, COUNT, AVG)
- Sorting (ORDER BY)
- Filtering (WHERE)

**Build with GPU support:**

```bash
# NVIDIA CUDA
cmake -B build -DSLOTHDB_CUDA=ON
cmake --build build

# Apple Metal
cmake -B build -DSLOTHDB_METAL=ON
cmake --build build
```

No code changes needed — the same SQL runs on CPU or GPU automatically.

---

## Extensions

SlothDB supports dynamic extensions via a stable C ABI. Extensions built for one version are guaranteed to work on future versions.

**Loading an extension:**

Extensions are loaded as shared libraries (`.so` / `.dll` / `.dylib`).

**Building an extension:**

```c
#include "slothdb/extension/extension_api.h"

SLOTHDB_EXTENSION_INIT(my_extension) {
    // Register custom functions, types, or table functions
    // using the extension API
}
```

See [`include/slothdb/extension/extension_api.h`](../include/slothdb/extension/extension_api.h) for the full extension API.

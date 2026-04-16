# SlothDB Documentation

The complete guide to SlothDB — query CSV, Parquet, JSON, and Excel files with SQL. No server, no setup, no dependencies.

---

## Table of Contents

| Section | What you'll learn |
|---------|-------------------|
| [1. Getting Started](#1-getting-started) | Install and run your first query in 30 seconds |
| [2. Query Your Files](#2-query-your-files) | Analyze CSV, Parquet, JSON, Excel, Avro, Arrow, and SQLite files |
| [3. Working with Large Datasets](#3-working-with-large-datasets) | Import, persist, and optimize queries on millions of rows |
| [4. SQL Guide](#4-sql-guide) | Tables, joins, window functions, CTEs, MERGE, and more |
| [5. All Functions](#5-all-functions) | 70+ built-in functions — string, math, date, aggregate, regex |
| [6. Python API](#6-python-api) | Use SlothDB from Python with pandas integration |
| [7. C/C++ API](#7-cc-api) | Embed SlothDB in C/C++ applications |
| [8. CLI Shell](#8-cli-shell) | Shell commands, flags, and tips |
| [9. GPU Acceleration](#9-gpu-acceleration) | CUDA and Metal for 20-100x faster analytics |
| [10. Extensions](#10-extensions) | Build and load custom extensions |

---

## 1. Getting Started

### Install

| Platform | Command |
|----------|---------|
| **Linux / macOS** | `curl -fsSL https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/install.sh \| bash` |
| **Windows** | Download [`slothdb.exe`](https://github.com/SouravRoy-ETL/slothdb/releases/latest) |
| **Python** | `pip install slothdb` |
| **Ubuntu / Debian** | `sudo dpkg -i slothdb_0.1.0_amd64.deb` ([download](https://github.com/SouravRoy-ETL/slothdb/releases/latest)) |
| **Fedora / RHEL** | `sudo rpm -i slothdb-0.1.0.rpm` (build from [spec](../packaging/rpm/slothdb.spec)) |
| **Arch Linux** | `makepkg -si` using the provided [PKGBUILD](../packaging/arch/PKGBUILD) |
| **macOS (Homebrew)** | `brew install --build-from-source packaging/homebrew/slothdb.rb` |
| **Build from source** | See [below](#build-from-source) |

### Your First Query

```bash
$ slothdb
slothdb> SELECT 'Hello, World!' AS greeting;
greeting
---------------
Hello, World!
```

That's it. You have a full SQL engine running. Now let's do something useful — query a real file:

```bash
slothdb> SELECT * FROM 'sales.csv' LIMIT 5;
```

SlothDB auto-detects the file format and runs SQL on it. No import step, no schema definition, no waiting.

### Build from Source

```bash
git clone https://github.com/SouravRoy-ETL/slothdb.git
cd slothdb
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/src/slothdb          # Linux/macOS
build\src\Release\slothdb.exe  # Windows
```

---

## 2. Query Your Files

This is what SlothDB is built for. Point SQL at any file and get results instantly. No importing, no schema setup, no extensions to install.

### CSV

```sql
-- Just query it
SELECT * FROM 'sales.csv';

-- Or use the explicit function
SELECT * FROM read_csv('sales.csv');

-- Real-world: aggregate a CSV without importing
SELECT department, COUNT(*) AS headcount, ROUND(AVG(salary)) AS avg_salary
FROM 'employees.csv'
GROUP BY department
ORDER BY avg_salary DESC;

-- Query multiple files at once with glob patterns
SELECT * FROM read_csv('logs/2024-*.csv');
```

### Parquet

Parquet is the **recommended format for large data**. SlothDB reads only the columns you SELECT and skips row groups that don't match your WHERE clause.

```sql
-- Query a Parquet file
SELECT * FROM read_parquet('events.parquet');

-- Only the 'user_id' and 'event' columns are read from disk
SELECT user_id, event FROM read_parquet('events.parquet') WHERE event = 'purchase';

-- Query partitioned Parquet datasets
SELECT * FROM read_parquet('data/year=2024/*.parquet');
```

**Why Parquet over CSV?**

| | CSV | Parquet |
|-|-----|---------|
| Read speed | Scans entire file | Reads only needed columns |
| File size | Raw text | 5-10x smaller (compressed) |
| Schema | Auto-detected (can be wrong) | Embedded in file (always correct) |
| Filter pushdown | No | Yes — skips non-matching row groups |

**Convert CSV to Parquet** for faster future queries:

```sql
COPY (SELECT * FROM 'huge_data.csv') TO 'huge_data.parquet' WITH (FORMAT PARQUET);

-- Now this runs much faster
SELECT category, SUM(amount) FROM read_parquet('huge_data.parquet') GROUP BY category;
```

### JSON

Supports both JSON arrays (`[{...}, {...}]`) and newline-delimited JSON (NDJSON — one object per line).

```sql
-- JSON array
SELECT * FROM read_json('users.json');

-- NDJSON (one JSON object per line — common in logging)
SELECT * FROM read_json('server_logs.ndjson');

-- Auto-detect
SELECT * FROM 'events.json';

-- Aggregate JSON data
SELECT status, COUNT(*) FROM 'api_responses.json' GROUP BY status;
```

### Excel

```sql
-- Read the first sheet
SELECT * FROM read_xlsx('quarterly_report.xlsx');

-- Auto-detect
SELECT * FROM 'quarterly_report.xlsx';

-- Analyze spreadsheet data with SQL
SELECT region, SUM(revenue) AS total
FROM 'sales_report.xlsx'
GROUP BY region
ORDER BY total DESC;
```

### Arrow IPC (Feather)

```sql
SELECT * FROM read_arrow('data.arrow');
SELECT * FROM read_arrow('data.feather');
```

### Avro

```sql
SELECT * FROM read_avro('events.avro');
```

### SQLite

Read tables directly from SQLite database files. No libsqlite3 needed — SlothDB reads the B-tree pages directly.

```sql
-- Read a table from a SQLite database
SELECT * FROM sqlite_scan('app.db', 'users');

-- Join SlothDB data with a SQLite table
SELECT e.name, s.score
FROM employees e
JOIN sqlite_scan('legacy_system.db', 'scores') s ON e.id = s.employee_id;
```

### Auto-Detection

When you use a string literal in the `FROM` clause, SlothDB detects the format by extension:

```sql
SELECT * FROM 'data.csv';        -- CSV
SELECT * FROM 'data.parquet';    -- Parquet
SELECT * FROM 'data.json';      -- JSON
SELECT * FROM 'report.xlsx';    -- Excel
SELECT * FROM 'data.arrow';     -- Arrow IPC
SELECT * FROM 'data.avro';      -- Avro
```

### Exporting Data

Write query results to any format:

```sql
-- Export to CSV
COPY employees TO 'backup.csv';

-- Export to Parquet (best for large data)
COPY employees TO 'backup.parquet' WITH (FORMAT PARQUET);

-- Export to JSON
COPY employees TO 'backup.json' WITH (FORMAT JSON);

-- Export filtered results
COPY (SELECT * FROM employees WHERE salary > 100000) TO 'top_earners.csv';

-- Custom CSV delimiter
COPY employees TO 'data.tsv' WITH (DELIMITER '\t', HEADER TRUE);
```

### Generate Sequences

```sql
-- Numbers 1 to 100
SELECT * FROM GENERATE_SERIES(1, 100);

-- Even numbers
SELECT * FROM GENERATE_SERIES(0, 100, 2);

-- Use in calculations
SELECT n, n * n AS square, SQRT(n) AS root
FROM GENERATE_SERIES(1, 20) gs(n);
```

---

## 3. Working with Large Datasets

### Strategy 1: Query Files Directly (simplest)

For one-off analysis, just query the file. SlothDB streams through it without loading everything into memory:

```sql
SELECT region, SUM(revenue)
FROM read_csv('10gb_sales.csv')
GROUP BY region;
```

### Strategy 2: Persistent Database (best for repeated queries)

If you'll query the same data multiple times, import it into a persistent database:

```bash
slothdb analytics.slothdb    # data persists across sessions
```

```sql
-- Import once
CREATE TABLE sales AS SELECT * FROM read_csv('sales_2024.csv');
CREATE TABLE events AS SELECT * FROM read_parquet('events.parquet');

-- Now queries are instant — no file parsing overhead
SELECT region, SUM(revenue) FROM sales GROUP BY region;
SELECT event_type, COUNT(*) FROM events GROUP BY event_type;
```

Next time you open the same `.slothdb` file, your tables are still there:

```bash
slothdb analytics.slothdb
slothdb> SELECT COUNT(*) FROM sales;  -- data persisted from last session
```

### Strategy 3: Convert to Parquet First (best for large CSVs)

If you have a large CSV you'll query often, convert it to Parquet once:

```sql
-- One-time conversion (CSV → Parquet)
COPY (SELECT * FROM read_csv('huge.csv')) TO 'huge.parquet' WITH (FORMAT PARQUET);

-- Every future query is 5-10x faster
SELECT category, COUNT(*) FROM read_parquet('huge.parquet') GROUP BY category;
```

### Strategy 4: Import Only What You Need

Don't import the entire dataset if you only need a subset:

```sql
-- Filter during import — only loads matching rows
CREATE TABLE recent AS
    SELECT * FROM read_csv('all_data.csv')
    WHERE year >= 2023 AND region = 'US';

-- Import specific columns only
CREATE TABLE summary AS
    SELECT product_id, SUM(qty) AS total_qty, SUM(revenue) AS total_revenue
    FROM read_parquet('transactions.parquet')
    GROUP BY product_id;
```

### Which Strategy to Use?

| Scenario | Best approach |
|----------|---------------|
| One-off analysis on a file | Query directly: `SELECT * FROM 'file.csv'` |
| Same data queried multiple times | Persistent database: `slothdb data.slothdb` |
| Huge CSV queried repeatedly | Convert to Parquet first |
| File that changes often | Create a view: `CREATE VIEW v AS SELECT * FROM read_csv('file.csv')` |
| Only need a subset of the data | Filter during import with `CREATE TABLE AS` |
| Multiple file formats, joined together | Import all into persistent DB, then join |

---

## 4. SQL Guide

### Data Types

| Type | Aliases | Description |
|------|---------|-------------|
| `BOOLEAN` | `BOOL` | `TRUE` / `FALSE` |
| `TINYINT` | `INT1` | 8-bit integer (-128 to 127) |
| `SMALLINT` | `INT2` | 16-bit integer |
| `INTEGER` | `INT`, `INT4` | 32-bit integer |
| `BIGINT` | `INT8` | 64-bit integer |
| `HUGEINT` | | 128-bit integer |
| `FLOAT` | `REAL`, `FLOAT4` | 32-bit float |
| `DOUBLE` | `FLOAT8` | 64-bit float |
| `DECIMAL(p,s)` | `NUMERIC` | Fixed-point decimal |
| `VARCHAR` | `TEXT`, `STRING` | Variable-length string |
| `BLOB` | `BYTEA` | Binary data |
| `DATE` | | Calendar date |
| `TIME` | | Time of day |
| `TIMESTAMP` | | Date and time (microsecond precision) |

### Creating Tables

```sql
-- Define a table with columns and constraints
CREATE TABLE employees (
    id INTEGER PRIMARY KEY,
    name VARCHAR NOT NULL,
    department VARCHAR,
    salary DOUBLE,
    hire_date DATE
);

CREATE TABLE IF NOT EXISTS employees (...);

-- Create a table from a query
CREATE TABLE top_earners AS
    SELECT * FROM employees WHERE salary > 100000;

-- Create a table from a file
CREATE TABLE logs AS SELECT * FROM read_csv('server_logs.csv');
CREATE TABLE events AS SELECT * FROM read_parquet('events.parquet');
CREATE TABLE users AS SELECT * FROM read_json('users.json');
```

### Modifying Tables

```sql
-- Add a column
ALTER TABLE employees ADD COLUMN email VARCHAR;

-- Remove a column
ALTER TABLE employees DROP COLUMN email;

-- Rename a column
ALTER TABLE employees RENAME COLUMN dept TO department;

-- Remove all rows (keep structure)
TRUNCATE TABLE employees;

-- Delete the table entirely
DROP TABLE employees;
DROP TABLE IF EXISTS employees;
```

### Views

Views are **virtual** — they re-execute the underlying query every time you access them. This means views on files always return fresh data.

```sql
-- View on a table
CREATE VIEW active_employees AS
    SELECT * FROM employees WHERE status = 'active';

-- View on a CSV file — always reads the latest data from disk
CREATE VIEW sales AS SELECT * FROM read_csv('sales.csv');

-- View on Parquet with filtering
CREATE VIEW recent_events AS
    SELECT * FROM read_parquet('events.parquet') WHERE event_date > '2024-01-01';

-- View on Excel
CREATE VIEW quarterly AS SELECT * FROM read_xlsx('Q4_report.xlsx');

-- View on SQLite
CREATE VIEW legacy_users AS SELECT * FROM sqlite_scan('old_app.db', 'users');

-- Now query views like tables — data is always fresh
SELECT region, SUM(revenue) FROM sales GROUP BY region;
SELECT COUNT(*) FROM recent_events;

-- Replace or drop views
CREATE OR REPLACE VIEW sales AS SELECT * FROM read_parquet('sales.parquet');
DROP VIEW sales;
DROP VIEW IF EXISTS sales;
```

**Why this matters:** If the underlying file changes (new rows added, updated data), the view automatically reflects it on the next query. No need to re-import or refresh.

### SELECT — Querying Data

```sql
-- All columns
SELECT * FROM employees;

-- Specific columns with aliases
SELECT name, salary * 12 AS annual_salary FROM employees;

-- Filtering
SELECT * FROM employees
WHERE department = 'Engineering' AND salary > 80000;

-- Sorting
SELECT * FROM employees ORDER BY salary DESC;
SELECT * FROM employees ORDER BY hire_date ASC NULLS LAST;

-- Pagination
SELECT * FROM employees ORDER BY id LIMIT 20 OFFSET 40;

-- Distinct values
SELECT DISTINCT department FROM employees;

-- Grouping with aggregation
SELECT department, COUNT(*) AS cnt, AVG(salary) AS avg_sal
FROM employees
GROUP BY department
HAVING AVG(salary) > 80000
ORDER BY avg_sal DESC;
```

### INSERT, UPDATE, DELETE

```sql
-- Insert rows
INSERT INTO employees VALUES (1, 'Alice', 'Engineering', 95000, '2022-01-15');

-- Insert multiple rows
INSERT INTO employees VALUES
    (2, 'Bob', 'Sales', 72000, '2021-06-01'),
    (3, 'Charlie', 'Engineering', 110000, '2020-03-22');

-- Insert specific columns
INSERT INTO employees (id, name, department) VALUES (4, 'Diana', 'Marketing');

-- Insert from a query
INSERT INTO archive SELECT * FROM employees WHERE hire_date < '2020-01-01';

-- Update rows
UPDATE employees SET salary = salary * 1.10 WHERE department = 'Engineering';

-- Delete rows
DELETE FROM employees WHERE id = 3;
DELETE FROM employees;  -- all rows
```

### MERGE (Upsert)

Insert or update in a single statement:

```sql
MERGE INTO employees AS target
USING new_hires AS source
ON target.id = source.id
WHEN MATCHED THEN
    UPDATE SET salary = source.salary, department = source.department
WHEN NOT MATCHED THEN
    INSERT (id, name, department, salary)
    VALUES (source.id, source.name, source.department, source.salary);
```

### Joins

```sql
-- INNER JOIN — only matching rows
SELECT e.name, d.dept_name
FROM employees e
INNER JOIN departments d ON e.dept_id = d.id;

-- LEFT JOIN — all employees, even without a department
SELECT e.name, d.dept_name
FROM employees e
LEFT JOIN departments d ON e.dept_id = d.id;

-- RIGHT JOIN — all departments, even without employees
SELECT e.name, d.dept_name
FROM employees e
RIGHT JOIN departments d ON e.dept_id = d.id;

-- FULL OUTER JOIN — all rows from both tables
SELECT e.name, d.dept_name
FROM employees e
FULL OUTER JOIN departments d ON e.dept_id = d.id;

-- CROSS JOIN — every combination
SELECT * FROM colors CROSS JOIN sizes;

-- NATURAL JOIN — auto-matches on same-named columns
SELECT * FROM orders NATURAL JOIN customers;

-- JOIN USING — shorthand when column names match
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

-- Join a table with a CSV file
SELECT e.name, s.score
FROM employees e
JOIN read_csv('scores.csv') s ON e.id = s.employee_id;
```

### Subqueries

```sql
-- In WHERE clause
SELECT * FROM employees
WHERE salary > (SELECT AVG(salary) FROM employees);

-- In FROM clause
SELECT dept, avg_salary
FROM (SELECT department AS dept, AVG(salary) AS avg_salary
      FROM employees GROUP BY department) sub
WHERE avg_salary > 80000;

-- EXISTS / NOT EXISTS
SELECT * FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id);

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

-- Recursive CTE — org chart traversal
WITH RECURSIVE org_chart(id, name, manager_id, level) AS (
    SELECT id, name, manager_id, 0 AS level
    FROM employees WHERE manager_id IS NULL
    UNION ALL
    SELECT e.id, e.name, e.manager_id, oc.level + 1
    FROM employees e JOIN org_chart oc ON e.manager_id = oc.id
)
SELECT * FROM org_chart ORDER BY level, name;

-- Recursive CTE — generate a sequence
WITH RECURSIVE nums(n) AS (
    SELECT 1
    UNION ALL
    SELECT n + 1 FROM nums WHERE n < 100
)
SELECT n FROM nums;
```

### Window Functions

Compute values across related rows without collapsing them — essential for ranking, running totals, and comparisons.

```sql
-- ROW_NUMBER — unique rank per partition
SELECT name, department, salary,
    ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) AS dept_rank
FROM employees;

-- RANK — same value = same rank, with gaps (1, 2, 2, 4)
SELECT name, salary,
    RANK() OVER (ORDER BY salary DESC) AS rank
FROM employees;

-- DENSE_RANK — same value = same rank, no gaps (1, 2, 2, 3)
SELECT name, salary,
    DENSE_RANK() OVER (ORDER BY salary DESC) AS dense_rank
FROM employees;

-- NTILE — split into N equal buckets
SELECT name, salary,
    NTILE(4) OVER (ORDER BY salary DESC) AS quartile
FROM employees;

-- LAG / LEAD — compare with previous/next row
SELECT date, revenue,
    revenue - LAG(revenue) OVER (ORDER BY date) AS daily_change,
    LEAD(revenue) OVER (ORDER BY date) AS tomorrow
FROM daily_sales;

-- FIRST_VALUE / LAST_VALUE
SELECT name, department, salary,
    FIRST_VALUE(name) OVER (PARTITION BY department ORDER BY salary DESC) AS top_earner
FROM employees;

-- Running total
SELECT date, amount,
    SUM(amount) OVER (ORDER BY date) AS running_total
FROM transactions;

-- Cumulative average
SELECT date, revenue,
    AVG(revenue) OVER (ORDER BY date) AS cumulative_avg
FROM daily_sales;
```

### QUALIFY — Filter on Window Results

Snowflake-style filtering on window functions. No subquery needed.

```sql
-- Top earner per department — one line instead of a subquery
SELECT name, department, salary
FROM employees
QUALIFY ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) = 1;

-- Top 3 products per category
SELECT product_name, category, revenue
FROM products
QUALIFY RANK() OVER (PARTITION BY category ORDER BY revenue DESC) <= 3;

-- Without QUALIFY, you'd need this:
SELECT * FROM (
    SELECT *, ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) AS rn
    FROM employees
) sub WHERE rn = 1;
```

### Set Operations

```sql
-- UNION — combine and deduplicate
SELECT name FROM employees_us UNION SELECT name FROM employees_eu;

-- UNION ALL — combine, keep duplicates (faster)
SELECT name FROM employees_us UNION ALL SELECT name FROM employees_eu;

-- INTERSECT — rows in both
SELECT customer_id FROM orders_2024 INTERSECT SELECT customer_id FROM orders_2025;

-- EXCEPT — rows in first but not second
SELECT customer_id FROM subscribers EXCEPT SELECT customer_id FROM unsubscribed;
```

### Transactions

```sql
BEGIN TRANSACTION;
INSERT INTO accounts VALUES (1, 'Alice', 5000);
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
COMMIT;

-- Undo changes
BEGIN;
DELETE FROM important_data;
ROLLBACK;  -- nothing was deleted
```

### EXPLAIN

See how SlothDB will execute your query:

```sql
EXPLAIN SELECT department, AVG(salary)
FROM employees WHERE hire_date > '2020-01-01'
GROUP BY department;

-- Shows: AGGREGATE → FILTER → SCAN employees
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

-- Pattern matching
SELECT * FROM employees WHERE name LIKE 'A%';          -- starts with A
SELECT * FROM employees WHERE name ILIKE 'alice';      -- case-insensitive
SELECT * FROM employees WHERE salary BETWEEN 50000 AND 100000;
SELECT * FROM employees WHERE department IN ('Engineering', 'Product');
SELECT * FROM employees WHERE manager_id IS NULL;
```

---

## 5. All Functions

### Aggregate Functions

| Function | Description | Example |
|----------|-------------|---------|
| `COUNT(*)` | Count all rows | `SELECT COUNT(*) FROM t` |
| `COUNT(col)` | Count non-NULL values | `SELECT COUNT(name) FROM t` |
| `COUNT(DISTINCT col)` | Count unique values | `SELECT COUNT(DISTINCT dept) FROM t` |
| `SUM(col)` | Sum | `SELECT SUM(salary) FROM t` |
| `AVG(col)` | Average | `SELECT AVG(salary) FROM t` |
| `MIN(col)` | Minimum | `SELECT MIN(hire_date) FROM t` |
| `MAX(col)` | Maximum | `SELECT MAX(salary) FROM t` |
| `STRING_AGG(col, sep)` | Concatenate strings | `SELECT STRING_AGG(name, ', ') FROM t` |
| `MEDIAN(col)` | Median value | `SELECT MEDIAN(salary) FROM t` |
| `STDDEV(col)` | Sample std deviation | `SELECT STDDEV(salary) FROM t` |
| `STDDEV_POP(col)` | Population std deviation | `SELECT STDDEV_POP(salary) FROM t` |
| `VARIANCE(col)` | Sample variance | `SELECT VARIANCE(salary) FROM t` |
| `VAR_POP(col)` | Population variance | `SELECT VAR_POP(salary) FROM t` |
| `BOOL_AND(col)` | Logical AND of all values | `SELECT BOOL_AND(active) FROM t` |
| `BOOL_OR(col)` | Logical OR of all values | `SELECT BOOL_OR(active) FROM t` |

**Aliases:** `GROUP_CONCAT`, `LISTAGG` = `STRING_AGG`. `STDDEV_SAMP` = `STDDEV`. `VAR_SAMP` = `VARIANCE`.

All aggregate functions also work as **window aggregates**:

```sql
SELECT name, salary,
    SUM(salary) OVER (PARTITION BY department) AS dept_total,
    AVG(salary) OVER (ORDER BY hire_date) AS running_avg
FROM employees;
```

### String Functions

| Function | What it does | Example → Result |
|----------|-------------|------------------|
| `LENGTH(s)` | String length | `LENGTH('hello')` → `5` |
| `UPPER(s)` | To uppercase | `UPPER('hello')` → `'HELLO'` |
| `LOWER(s)` | To lowercase | `LOWER('HELLO')` → `'hello'` |
| `CONCAT(s1, s2, ...)` | Join strings | `CONCAT('a', 'b')` → `'ab'` |
| `s1 \|\| s2` | Join strings (operator) | `'hi' \|\| ' there'` → `'hi there'` |
| `SUBSTRING(s, pos, len)` | Extract part | `SUBSTRING('hello', 2, 3)` → `'ell'` |
| `REPLACE(s, from, to)` | Replace text | `REPLACE('foo', 'o', 'a')` → `'faa'` |
| `TRIM(s)` | Remove whitespace | `TRIM('  hi  ')` → `'hi'` |
| `LTRIM(s)` | Remove left whitespace | `LTRIM('  hi')` → `'hi'` |
| `RTRIM(s)` | Remove right whitespace | `RTRIM('hi  ')` → `'hi'` |
| `LEFT(s, n)` | First n characters | `LEFT('hello', 3)` → `'hel'` |
| `RIGHT(s, n)` | Last n characters | `RIGHT('hello', 3)` → `'llo'` |
| `LPAD(s, len, pad)` | Pad from left | `LPAD('42', 5, '0')` → `'00042'` |
| `RPAD(s, len, pad)` | Pad from right | `RPAD('hi', 5, '.')` → `'hi...'` |
| `REVERSE(s)` | Reverse | `REVERSE('hello')` → `'olleh'` |
| `REPEAT(s, n)` | Repeat n times | `REPEAT('ha', 3)` → `'hahaha'` |
| `POSITION(sub IN s)` | Find position | `POSITION('ll' IN 'hello')` → `3` |
| `STARTS_WITH(s, pre)` | Starts with? | `STARTS_WITH('hello', 'he')` → `true` |
| `ENDS_WITH(s, suf)` | Ends with? | `ENDS_WITH('hello', 'lo')` → `true` |
| `CONTAINS(s, sub)` | Contains? | `CONTAINS('hello', 'ell')` → `true` |
| `SPLIT_PART(s, d, i)` | Split and pick part | `SPLIT_PART('a-b-c', '-', 2)` → `'b'` |
| `INITCAP(s)` | Capitalize words | `INITCAP('hello world')` → `'Hello World'` |

**Aliases:** `CHAR_LENGTH` = `LENGTH`. `SUBSTR` = `SUBSTRING`. `STRPOS` = `POSITION`. `PREFIX` = `STARTS_WITH`. `SUFFIX` = `ENDS_WITH`.

### Math Functions

| Function | What it does | Example → Result |
|----------|-------------|------------------|
| `ABS(x)` | Absolute value | `ABS(-5)` → `5` |
| `CEIL(x)` | Round up | `CEIL(3.2)` → `4` |
| `FLOOR(x)` | Round down | `FLOOR(3.8)` → `3` |
| `ROUND(x)` | Round to nearest | `ROUND(3.5)` → `4` |
| `TRUNC(x)` | Truncate decimal | `TRUNC(3.9)` → `3` |
| `SQRT(x)` | Square root | `SQRT(16)` → `4` |
| `POWER(x, y)` | x to the power y | `POWER(2, 10)` → `1024` |
| `MOD(x, y)` | Remainder | `MOD(10, 3)` → `1` |
| `LOG(x)` | Natural log (ln) | `LOG(2.718)` → `~1.0` |
| `LOG2(x)` | Log base 2 | `LOG2(8)` → `3` |
| `LOG10(x)` | Log base 10 | `LOG10(100)` → `2` |
| `EXP(x)` | e^x | `EXP(1)` → `2.718...` |
| `SIGN(x)` | Sign (-1, 0, 1) | `SIGN(-42)` → `-1` |
| `PI()` | Pi constant | `PI()` → `3.14159...` |
| `RANDOM()` | Random [0, 1) | `RANDOM()` → `0.7231...` |
| `LEAST(a, b, ...)` | Smallest value | `LEAST(5, 3, 9)` → `3` |
| `GREATEST(a, b, ...)` | Largest value | `GREATEST(5, 3, 9)` → `9` |

**Aliases:** `CEILING` = `CEIL`. `LN` = `LOG`. `TRUNCATE` = `TRUNC`. `RAND` = `RANDOM`.

### Trigonometric Functions

| Function | Description |
|----------|-------------|
| `SIN(x)`, `COS(x)`, `TAN(x)` | Trig functions (radians) |
| `ASIN(x)`, `ACOS(x)`, `ATAN(x)` | Inverse trig |
| `ATAN2(y, x)` | Two-argument arctangent |
| `DEGREES(x)` | Radians → degrees |
| `RADIANS(x)` | Degrees → radians |

### Date/Time Functions

| Function | What it does | Example |
|----------|-------------|---------|
| `NOW()` | Current timestamp | `SELECT NOW()` |
| `CURRENT_TIMESTAMP` | Current timestamp | `SELECT CURRENT_TIMESTAMP` |
| `CURRENT_DATE` | Current date | `SELECT CURRENT_DATE` |
| `EXTRACT(part FROM ts)` | Get year/month/day/etc. | `EXTRACT(YEAR FROM ts)` |
| `DATE_PART(part, ts)` | Same as EXTRACT | `DATE_PART('month', ts)` |
| `DATE_ADD(part, n, ts)` | Add time interval | `DATE_ADD('day', 7, ts)` |
| `DATE_DIFF(part, t1, t2)` | Time between two dates | `DATE_DIFF('day', start, end)` |
| `DATE_TRUNC(part, ts)` | Truncate to unit | `DATE_TRUNC('month', ts)` |
| `STRFTIME(fmt, ts)` | Format as string | `STRFTIME('%Y-%m-%d', ts)` |
| `TO_TIMESTAMP(epoch)` | Epoch → timestamp | `TO_TIMESTAMP(1700000000)` |
| `EPOCH_MS(ts)` | Timestamp → epoch ms | `EPOCH_MS(ts)` |

**EXTRACT parts:** `YEAR`, `MONTH`, `DAY`, `HOUR`, `MINUTE`, `SECOND`, `EPOCH`, `DOW`

**DATE_ADD / DATE_DIFF parts:** `DAY`, `HOUR`, `MINUTE`, `SECOND`

**Aliases:** `DATEADD` = `DATE_ADD`. `DATEDIFF` = `DATE_DIFF`. `FORMAT_TIMESTAMP` = `STRFTIME`. `MAKE_TIMESTAMP` = `TO_TIMESTAMP`.

### Null Handling

| Function | What it does | Example |
|----------|-------------|---------|
| `COALESCE(a, b, ...)` | First non-NULL value | `COALESCE(phone, email, 'N/A')` |
| `NULLIF(a, b)` | NULL if a = b | `NULLIF(divisor, 0)` — prevents division by zero |

### Regex Functions

| Function | What it does | Example |
|----------|-------------|---------|
| `REGEXP_MATCHES(s, pat)` | Does pattern match? | `REGEXP_MATCHES(email, '.*@.*\.com')` |
| `REGEXP_REPLACE(s, pat, r)` | Replace matches | `REGEXP_REPLACE(phone, '[^0-9]', '')` |
| `REGEXP_EXTRACT(s, pat)` | Extract first match | `REGEXP_EXTRACT(url, 'https?://([^/]+)')` |

**Alias:** `REGEXP_MATCH` = `REGEXP_MATCHES`.

### Type Casting

```sql
-- CAST — errors on invalid input
SELECT CAST('42' AS INTEGER);
SELECT CAST(3.14 AS VARCHAR);

-- TRY_CAST — returns NULL instead of error
SELECT TRY_CAST('not_a_number' AS INTEGER);  -- NULL
SELECT TRY_CAST('42' AS INTEGER);            -- 42
```

---

## 6. Python API

### Install

```bash
pip install slothdb
```

### Quick Start

```python
import slothdb

# Connect (in-memory)
db = slothdb.connect()

# Connect (persistent — saves to file)
db = slothdb.connect("analytics.slothdb")
```

### Query Files Directly

```python
import slothdb

db = slothdb.connect()

# Query a CSV file
result = db.sql("SELECT * FROM 'sales.csv' LIMIT 10")
print(result)

# Aggregate a Parquet file
result = db.sql("""
    SELECT region, SUM(revenue) AS total
    FROM read_parquet('sales.parquet')
    GROUP BY region
    ORDER BY total DESC
""")
print(result)
```

### Create Tables and Run Queries

```python
db = slothdb.connect("my.slothdb")

# Create table
db.execute("CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER)")
db.execute("INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25)")

# Query
result = db.sql("SELECT * FROM users WHERE age > 20")
print(result)
# name       | age
# -----------+-----------
# Alice      | 30
# Bob        | 25
```

### Working with Results

```python
result = db.sql("SELECT name, age FROM users")

# Metadata
result.column_names   # ['name', 'age']
result.column_count   # 2
result.row_count      # number of rows
len(result)           # same as row_count

# Fetch data
result.fetchone()     # first row as tuple: ('Alice', 30)
result.fetchall()     # all rows as list of tuples
```

### Pandas Integration

```python
import slothdb

db = slothdb.connect()
result = db.sql("""
    SELECT region, SUM(revenue) AS total
    FROM read_csv('sales.csv')
    GROUP BY region
""")

df = result.fetchdf()  # pandas DataFrame
print(df)
#     region    total
# 0  US-East  1250000
# 1  US-West   980000
# 2   Europe   730000
```

### Context Manager

```python
with slothdb.connect("analytics.slothdb") as db:
    db.execute("INSERT INTO logs VALUES (1, 'click', NOW())")
    result = db.sql("SELECT COUNT(*) FROM logs")
    print(result.fetchone())
# connection is automatically closed
```

### End-to-End Example

```python
import slothdb

db = slothdb.connect("company.slothdb")

# Load CSV into a persistent table
db.execute("""
    CREATE TABLE IF NOT EXISTS employees AS
    SELECT * FROM read_csv('employees.csv')
""")

# Analytics with window functions
result = db.sql("""
    SELECT name, department, salary,
        AVG(salary) OVER (PARTITION BY department) AS dept_avg,
        RANK() OVER (PARTITION BY department ORDER BY salary DESC) AS rank
    FROM employees
    ORDER BY department, rank
""")

# To pandas
df = result.fetchdf()
print(df.head(20))

# Export to Parquet
db.execute("""
    COPY (SELECT * FROM employees WHERE hire_date >= '2023-01-01')
    TO 'recent_hires.parquet' WITH (FORMAT PARQUET)
""")

db.close()
```

### Python API Reference

| Method | Returns | Description |
|--------|---------|-------------|
| `slothdb.connect(path="")` | `Connection` | Connect to a database. Empty = in-memory. |
| `conn.sql(query)` | `QueryResult` | Execute query and return results |
| `conn.execute(query)` | `QueryResult` | Execute statement (alias for sql) |
| `conn.close()` | — | Close the connection |
| `result.column_names` | `list[str]` | Column names |
| `result.column_count` | `int` | Number of columns |
| `result.row_count` | `int` | Number of rows |
| `result.fetchone()` | `tuple` | First row |
| `result.fetchall()` | `list[tuple]` | All rows |
| `result.fetchdf()` | `DataFrame` | Convert to pandas (requires pandas) |

---

## 7. C/C++ API

### Quick Start

```c
#include "slothdb/api/slothdb.h"

slothdb_database *db;
slothdb_connection *conn;
slothdb_result *result;

slothdb_open("analytics.slothdb", &db);   // or "" for in-memory
slothdb_connect(db, &conn);
slothdb_query(conn, "SELECT 42 AS answer", &result);

printf("Answer: %d\n", slothdb_value_int32(result, 0, 0));

slothdb_free_result(result);
slothdb_disconnect(conn);
slothdb_close(db);
```

### Query and Read Results

```c
slothdb_result *result;
slothdb_status status = slothdb_query(conn,
    "SELECT name, salary FROM employees ORDER BY salary DESC", &result);

if (status != SLOTHDB_OK) {
    fprintf(stderr, "Error: %s\n", slothdb_result_error(result));
    slothdb_free_result(result);
    return;
}

uint64_t rows = slothdb_row_count(result);
uint64_t cols = slothdb_column_count(result);

// Print headers
for (uint64_t c = 0; c < cols; c++)
    printf("%-20s", slothdb_column_name(result, c));
printf("\n");

// Print rows
for (uint64_t r = 0; r < rows; r++) {
    for (uint64_t c = 0; c < cols; c++) {
        if (slothdb_value_is_null(result, r, c))
            printf("%-20s", "NULL");
        else
            printf("%-20s", slothdb_value_varchar(result, r, c));
    }
    printf("\n");
}

slothdb_free_result(result);
```

### Query Files from C

```c
// Query a CSV file
slothdb_query(conn, "SELECT * FROM read_csv('data.csv')", &result);

// Query Parquet
slothdb_query(conn,
    "SELECT region, SUM(revenue) FROM read_parquet('sales.parquet') GROUP BY region",
    &result);

// Load file into a table
slothdb_query(conn,
    "CREATE TABLE events AS SELECT * FROM read_parquet('events.parquet')",
    &result);
slothdb_free_result(result);
```

### Error Handling

```c
slothdb_status status = slothdb_query(conn, sql, &result);

switch (status) {
    case SLOTHDB_OK:      /* success */                                    break;
    case SLOTHDB_ERROR:   fprintf(stderr, "%s\n", slothdb_result_error(result)); break;
    case SLOTHDB_INVALID: fprintf(stderr, "Invalid argument\n");           break;
}

slothdb_free_result(result);  // always free, even on error
```

### Value Accessor Functions

| Function | Returns | Use for |
|----------|---------|---------|
| `slothdb_value_int32(result, row, col)` | `int32_t` | INTEGER columns |
| `slothdb_value_int64(result, row, col)` | `int64_t` | BIGINT columns |
| `slothdb_value_double(result, row, col)` | `double` | FLOAT / DOUBLE columns |
| `slothdb_value_varchar(result, row, col)` | `const char*` | Any column as string |
| `slothdb_value_is_null(result, row, col)` | `int` | 1 if NULL, 0 otherwise |

### Result Metadata Functions

| Function | Returns | Description |
|----------|---------|-------------|
| `slothdb_column_count(result)` | `uint64_t` | Number of columns |
| `slothdb_row_count(result)` | `uint64_t` | Number of rows |
| `slothdb_column_name(result, col)` | `const char*` | Column name |
| `slothdb_column_type(result, col)` | `slothdb_type` | Column type enum |
| `slothdb_result_error(result)` | `const char*` | Error message |
| `slothdb_version()` | `const char*` | Version string |

### Type Enum

| Constant | Value |
|----------|-------|
| `SLOTHDB_TYPE_BOOLEAN` | 2 |
| `SLOTHDB_TYPE_INTEGER` | 5 |
| `SLOTHDB_TYPE_BIGINT` | 6 |
| `SLOTHDB_TYPE_FLOAT` | 11 |
| `SLOTHDB_TYPE_DOUBLE` | 12 |
| `SLOTHDB_TYPE_VARCHAR` | 15 |

### Full C Example

```c
#include <stdio.h>
#include "slothdb/api/slothdb.h"

int main() {
    slothdb_database *db;
    slothdb_connection *conn;
    slothdb_result *result;

    slothdb_open("company.slothdb", &db);
    slothdb_connect(db, &conn);

    // Create and populate
    slothdb_query(conn,
        "CREATE TABLE IF NOT EXISTS employees ("
        "  id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, salary DOUBLE"
        ")", &result);
    slothdb_free_result(result);

    slothdb_query(conn,
        "INSERT INTO employees VALUES "
        "(1, 'Alice', 95000), (2, 'Bob', 87000), (3, 'Charlie', 110000)",
        &result);
    slothdb_free_result(result);

    // Query with window function
    slothdb_status s = slothdb_query(conn,
        "SELECT name, salary, RANK() OVER (ORDER BY salary DESC) AS rank "
        "FROM employees", &result);

    if (s == SLOTHDB_OK) {
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

### C++ RAII Wrapper

```cpp
#include <iostream>
#include <stdexcept>
#include "slothdb/api/slothdb.h"

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
        slothdb_result *r;
        if (slothdb_query(conn_, sql, &r) != SLOTHDB_OK) {
            std::string err = slothdb_result_error(r);
            slothdb_free_result(r);
            throw std::runtime_error(err);
        }
        slothdb_free_result(r);
    }

    void query(const char *sql) {
        slothdb_result *r;
        if (slothdb_query(conn_, sql, &r) != SLOTHDB_OK) {
            std::string err = slothdb_result_error(r);
            slothdb_free_result(r);
            throw std::runtime_error(err);
        }
        for (uint64_t c = 0; c < slothdb_column_count(r); c++)
            std::cout << slothdb_column_name(r, c) << "\t";
        std::cout << "\n";
        for (uint64_t row = 0; row < slothdb_row_count(r); row++) {
            for (uint64_t c = 0; c < slothdb_column_count(r); c++)
                std::cout << (slothdb_value_is_null(r, row, c)
                    ? "NULL" : slothdb_value_varchar(r, row, c)) << "\t";
            std::cout << "\n";
        }
        slothdb_free_result(r);
    }
};

int main() {
    SlothDB db("analytics.slothdb");
    db.execute("CREATE TABLE events AS SELECT * FROM read_csv('events.csv')");
    db.query("SELECT event_type, COUNT(*) FROM events GROUP BY event_type ORDER BY COUNT(*) DESC LIMIT 10");
}
```

### Building with CMake

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_app)

# Option A: as subdirectory
add_subdirectory(slothdb)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE slothdb_lib)

# Option B: find installed library
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

## 8. CLI Shell

### Running SlothDB

```bash
slothdb                          # in-memory database
slothdb analytics.slothdb        # persistent database (creates file if needed)
slothdb -c "SELECT 42"           # run one query and exit
```

### Shell Dot-Commands

| Command | Description |
|---------|-------------|
| `.help` | Show available commands |
| `.quit` / `.exit` | Exit the shell |
| `.tables` | List all tables |
| `.schema` | Show table schemas |
| `.version` | Show SlothDB version |

### Practical Shell Examples

**Explore a CSV file:**

```
$ slothdb
slothdb> SELECT * FROM 'sales.csv' LIMIT 5;
slothdb> SELECT COUNT(*) FROM 'sales.csv';
slothdb> SELECT region, SUM(revenue) FROM 'sales.csv' GROUP BY region;
slothdb> .quit
```

**Build a persistent analytics database:**

```
$ slothdb analytics.slothdb
slothdb> CREATE TABLE sales AS SELECT * FROM read_csv('sales_2024.csv');
slothdb> CREATE TABLE users AS SELECT * FROM read_json('users.json');
slothdb> SELECT u.name, SUM(s.amount)
   ...>   FROM sales s JOIN users u ON s.user_id = u.id
   ...>   GROUP BY u.name ORDER BY SUM(s.amount) DESC LIMIT 10;
slothdb> .quit

$ slothdb analytics.slothdb    # next time, tables are still there
slothdb> .tables
slothdb> SELECT COUNT(*) FROM sales;
```

**Quick one-liners from the terminal:**

```bash
# Count rows in a CSV
slothdb -c "SELECT COUNT(*) FROM 'data.csv'"

# Top 5 departments by average salary
slothdb -c "SELECT dept, AVG(salary) FROM 'employees.parquet' GROUP BY dept ORDER BY 2 DESC LIMIT 5"

# Convert CSV to Parquet
slothdb -c "COPY (SELECT * FROM 'big.csv') TO 'big.parquet' WITH (FORMAT PARQUET)"
```

---

## 9. GPU Acceleration

SlothDB automatically uses your GPU when the dataset exceeds 100,000 rows. No code changes needed — the same SQL runs faster.

**Supported GPUs:**
- **NVIDIA** — via CUDA
- **Apple Silicon** (M1/M2/M3/M4) — via Metal
- No GPU? Automatic CPU fallback.

**What gets accelerated:**
- `GROUP BY` aggregations
- `ORDER BY` sorting
- `WHERE` filtering

**Build with GPU support:**

```bash
cmake -B build -DSLOTHDB_CUDA=ON     # NVIDIA
cmake -B build -DSLOTHDB_METAL=ON    # Apple Silicon
cmake --build build
```

---

## 10. Extensions

SlothDB supports custom extensions via a **stable C ABI**. Extensions built for v1.0 are guaranteed to work on v2.0 and beyond.

**Building an extension:**

```c
#include "slothdb/extension/extension_api.h"

SLOTHDB_EXTENSION_INIT(my_extension) {
    // Register custom functions, types, or table functions
}
```

Extensions are loaded as shared libraries (`.so` / `.dll` / `.dylib`).

See [`include/slothdb/extension/extension_api.h`](../include/slothdb/extension/extension_api.h) for the full API.

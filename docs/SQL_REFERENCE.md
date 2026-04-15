# SlothDB SQL Reference

## Data Types

| Type | Aliases | Description |
|------|---------|-------------|
| `BOOLEAN` | `BOOL` | True/false |
| `TINYINT` | `INT1` | 8-bit integer |
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
| `TIMESTAMP` | | Date and time |
| `BOOLEAN` | `BOOL` | True/false |

## Statements

### CREATE TABLE
```sql
CREATE TABLE name (
    column1 TYPE [NOT NULL] [PRIMARY KEY],
    column2 TYPE,
    ...
);
CREATE TABLE IF NOT EXISTS name (...);
```

### DROP TABLE
```sql
DROP TABLE name;
DROP TABLE IF EXISTS name;
```

### ALTER TABLE
```sql
ALTER TABLE name ADD COLUMN col_name TYPE;
ALTER TABLE name DROP COLUMN col_name;
ALTER TABLE name RENAME COLUMN old_name TO new_name;
```

### INSERT
```sql
INSERT INTO table VALUES (val1, val2, ...);
INSERT INTO table VALUES (v1, v2), (v3, v4);
INSERT INTO table (col1, col2) VALUES (v1, v2);
INSERT INTO table SELECT ... FROM other_table;
```

### UPDATE
```sql
UPDATE table SET col1 = val1, col2 = val2 WHERE condition;
```

### DELETE
```sql
DELETE FROM table WHERE condition;
DELETE FROM table;  -- delete all rows
```

### MERGE
```sql
MERGE INTO target USING source ON target.id = source.id
    WHEN MATCHED THEN UPDATE SET col = source.col
    WHEN NOT MATCHED THEN INSERT (cols) VALUES (vals);
```

### SELECT
```sql
SELECT [DISTINCT] expr [AS alias], ...
FROM table [alias]
    [JOIN table ON condition]
WHERE condition
GROUP BY expr, ...
HAVING condition
QUALIFY window_condition
ORDER BY expr [ASC|DESC] [NULLS FIRST|LAST]
LIMIT n [OFFSET m];
```

### WITH (CTE)
```sql
WITH cte_name AS (SELECT ...)
SELECT * FROM cte_name;

WITH RECURSIVE cte(cols) AS (
    base_case
    UNION ALL
    recursive_case
)
SELECT * FROM cte;
```

### Set Operations
```sql
SELECT ... UNION [ALL] SELECT ...
SELECT ... INTERSECT SELECT ...
SELECT ... EXCEPT SELECT ...
```

### COPY
```sql
COPY table TO 'file.csv';
COPY table TO 'file.json' WITH (FORMAT JSON);
COPY table TO 'file.parquet' WITH (FORMAT PARQUET);
COPY table FROM 'file.csv';
COPY table FROM 'file.json' WITH (FORMAT JSON);
```

### Transactions
```sql
BEGIN [TRANSACTION];
COMMIT [TRANSACTION];
ROLLBACK [TRANSACTION];
```

### Other
```sql
EXPLAIN SELECT ...;
CREATE VIEW name AS SELECT ...;
CREATE OR REPLACE VIEW name AS SELECT ...;
TRUNCATE TABLE name;
```

## File I/O Functions

| Function | Description |
|----------|-------------|
| `read_csv('path')` | Read CSV file |
| `read_json('path')` | Read JSON/NDJSON file |
| `read_parquet('path')` | Read Parquet file |
| `read_arrow('path')` | Read Arrow IPC file |
| `read_avro('path')` | Read Avro file |
| `read_xlsx('path')` | Read Excel file |
| `sqlite_scan('db', 'table')` | Read SQLite table |
| `GENERATE_SERIES(start, stop [, step])` | Generate integer sequence |

Auto-detect: `SELECT * FROM 'file.csv'` (detects format by extension).

Glob patterns: `SELECT * FROM read_csv('data/*.csv')`.

## Scalar Functions

### String
`LENGTH`, `UPPER`, `LOWER`, `CONCAT`, `||`, `SUBSTRING(s, start, len)`, `REPLACE(s, from, to)`, `TRIM`, `LTRIM`, `RTRIM`, `POSITION(s, sub)`, `LEFT(s, n)`, `RIGHT(s, n)`, `LPAD(s, len, pad)`, `RPAD(s, len, pad)`, `REVERSE`, `REPEAT(s, n)`, `STARTS_WITH(s, prefix)`, `ENDS_WITH(s, suffix)`, `CONTAINS(s, sub)`, `SPLIT_PART(s, delim, idx)`, `INITCAP`

### Math
`ABS`, `CEIL`, `FLOOR`, `ROUND`, `SQRT`, `POWER(base, exp)`, `MOD`, `%`, `LOG/LN`, `LOG2`, `LOG10`, `EXP`, `SIGN`, `PI()`, `RANDOM()`, `LEAST(a, b, ...)`, `GREATEST(a, b, ...)`, `SIN`, `COS`, `TAN`, `ASIN`, `ACOS`, `ATAN`, `ATAN2`, `DEGREES`, `RADIANS`, `TRUNC`

### Date/Time
`NOW()`, `CURRENT_TIMESTAMP`, `CURRENT_DATE`, `EXTRACT(part FROM ts)`, `DATE_ADD(part, n, ts)`, `DATE_DIFF(part, ts1, ts2)`, `STRFTIME(ts)`, `TO_TIMESTAMP(epoch_sec)`

### Null
`COALESCE(a, b, ...)`, `NULLIF(a, b)`

### Regex
`REGEXP_MATCHES(s, pattern)`, `REGEXP_REPLACE(s, pattern, replacement)`, `REGEXP_EXTRACT(s, pattern)`

### Type
`CAST(x AS type)`, `TRY_CAST(x AS type)`

## Aggregate Functions

`COUNT(*)`, `COUNT(col)`, `COUNT(DISTINCT col)`, `SUM`, `AVG`, `MIN`, `MAX`, `STRING_AGG(col, delim)`, `STDDEV`, `STDDEV_POP`, `VARIANCE`, `VAR_POP`, `MEDIAN`, `BOOL_AND`, `BOOL_OR`

## Window Functions

```sql
func() OVER (
    [PARTITION BY expr, ...]
    [ORDER BY expr [ASC|DESC], ...]
)
```

Functions: `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `NTILE(n)`, `LEAD(col [, offset])`, `LAG(col [, offset])`, `FIRST_VALUE(col)`, `LAST_VALUE(col)`, `SUM(col) OVER (...)`, `COUNT(*) OVER (...)`, `AVG(col) OVER (...)`

### QUALIFY
```sql
SELECT *, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary DESC)
FROM employees
QUALIFY ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary DESC) = 1;
```

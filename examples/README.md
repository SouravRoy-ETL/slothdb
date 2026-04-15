# Sample Data

Use these files to try SlothDB:

```bash
# Start SlothDB in this directory
cd examples
../slothdb_shell

# Query CSV
slothdb> SELECT * FROM 'employees.csv';
slothdb> SELECT department, COUNT(*), AVG(salary) FROM 'employees.csv' GROUP BY department;

# Query JSON
slothdb> SELECT customer, SUM(amount * quantity) AS total FROM 'orders.json' GROUP BY customer ORDER BY total DESC;

# Top earner per department
slothdb> SELECT name, department, salary,
   ...>   ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC)
   ...> FROM 'employees.csv'
   ...> QUALIFY ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) = 1;

# Join CSV and JSON
slothdb> CREATE TABLE emp AS SELECT * FROM 'employees.csv';
slothdb> CREATE TABLE orders AS SELECT * FROM 'orders.json';
slothdb> SELECT e.name, e.department, COUNT(o.order_id) AS num_orders
   ...> FROM emp e LEFT JOIN orders o ON e.name = o.customer
   ...> GROUP BY e.name, e.department;

# Export to Parquet
slothdb> COPY emp TO 'employees.parquet' WITH (FORMAT PARQUET);
slothdb> SELECT * FROM 'employees.parquet';
```

# `.ask` — Natural-language queries in the SlothDB shell

<p align="center">
  <img src="../assets/ask-demo.svg" alt="slothdb .ask demo — natural-language queries translated to SQL" width="100%">
</p>

`.ask` translates plain English into SQL, shows you the SQL, and prompts before running. It's **pure rules** — no model weights, no network, no telemetry. Around 50 KB of C++ inside the regular SlothDB binary.

## Two modes

**Single-shot:**

```
slothdb> .ask how many sales in 2024
-- SELECT COUNT(*) FROM "sales" WHERE "order_date" LIKE '2024%'
Run? [Y/n] y
3
```

**Interactive** — enter by typing `.ask` alone. Keep asking without re-prefixing. `exit` / `quit` / blank line / Ctrl-D leaves.

```
slothdb> .ask
ask mode — type a question in English.
  Commands: `exit` / `quit` / blank line / Ctrl+D to leave.

ask> find me the latest sales
-- SELECT * FROM "sales" ORDER BY "order_date" DESC LIMIT 1
Run? [Y/n] y
…
ask> which region had the most customer_id
-- SELECT "region", COUNT(DISTINCT "customer_id") AS "n" FROM "sales" GROUP BY "region" ORDER BY COUNT(DISTINCT "customer_id") DESC LIMIT 1
Run? [Y/n] y
…
ask> exit
Back to SQL mode.
slothdb>
```

## Supported phrasings

| Shape | Example | Generates |
|---|---|---|
| Count | `how many sales`, `count of sales`, `number of sales` | `COUNT(*)` |
| Count + year | `how many sales in 2024` | adds `WHERE date LIKE '2024%'` |
| Sum | `total amount`, `sum of amount` | `SUM(amount)` |
| Sum + group | `total amount per region` | adds `GROUP BY region` |
| Avg / Min / Max | `average amount`, `min amount`, `max amount` | `AVG` / `MIN` / `MAX` |
| Top-N | `top 5 customer_id by amount`, `bottom 3 region by amount` | `ORDER BY … DESC/ASC LIMIT N` |
| Latest / Oldest | `find me the latest sales`, `most recent sales`, `oldest sales` | `ORDER BY date_col DESC/ASC LIMIT 1` |
| Superlative | `which region had the most customer_id`, `which month had most orders` | `GROUP BY key` + `COUNT(DISTINCT)` or `SUM`, `ORDER BY … DESC LIMIT 1` |
| Select-all | `rows from sales` | `SELECT * FROM sales LIMIT 100` |

Year filtering (`in 2024`, `during 2024`) auto-detects a `date`-typed or `*_date` / `*_at` column. If there isn't one, the filter is skipped silently — only the underlying query runs.

## Heuristics you should know about

- **Singular ↔ plural**: `sale` resolves to `sales` (and back). This matters because different users phrase the same question differently.
- **ID-column detection**: a metric named `id` or ending in `_id` is treated as an identifier, not a value. `which region had most customer_id` uses `COUNT(DISTINCT customer_id)`, not `SUM` — because summing IDs is meaningless.
- **Exact match before synonym**: `total price` on a two-table schema with `sales.amount` and `products.price` routes to `products`, not `sales` — even though synonyms would route `price → amount`. Exact column-name wins.
- **Filler words stripped**: `find`, `me`, `show`, `give`, `tell`, `list`, `which`, `who`, `had`, `has`, `please`, `all`, `the` — all stripped before matching. So *"find me the latest sales"* is equivalent to *"latest sales"*.
- **Month / year / week / day grouping** on a **typed DATE** column uses `EXTRACT`. On a **VARCHAR** column we fall back to grouping by the whole string — SlothDB's planner doesn't currently accept function expressions in `GROUP BY`, so the coarser grouping keeps the query correct. Once that lands we'll tighten to `SUBSTRING(date, 1, 7)` for monthly, etc.

## Column-name synonyms

If the NL noun doesn't match a column exactly, `.ask` tries a small synonym table:

| You say | We look for |
|---|---|
| `revenue` | `revenue`, `total`, `amount`, `value`, `price`, `sales`, `sum` |
| `amount` | `amount`, `total`, `value`, `price` |
| `total` | `total`, `amount`, `sum`, `revenue` |
| `value` | `value`, `amount`, `total` |
| `price` | `price`, `amount`, `cost` |
| `cost` | `cost`, `price`, `amount` |
| `customer` | `customer`, `client`, `buyer`, `user` |
| `product` | `product`, `item`, `sku` |
| `date` / `month` / `year` | `date`, `day`, `time`, `timestamp`, `created_at`, `order_date` |
| `region` | `region`, `country`, `location`, `area` |

## What `.ask` won't do (on purpose)

The rule engine is narrow by design — it refuses rather than hallucinate SQL it doesn't understand. Specifically:

- **Multi-table JOINs** — if your question implicitly joins two tables ("customer names with their total spend"), `.ask` refuses. Write the JOIN yourself.
- **Genuinely ambiguous phrasings** — "who are my most loyal repeat customers", "which products are trending" — these don't have a single correct SQL answer. The refusal message points you at `.schema` and the supported list here.
- **Running totals / cumulative sums** — the window-function SQL is standard, but SlothDB's current engine doesn't execute unbounded-frame windows correctly. `.ask` refuses with a pointer to that gap rather than generate misleading output.
- **Full-text search / fuzzy matching on values** — no `LIKE '%foo%'` on arbitrary text unless you name the column and value explicitly (which is just SQL anyway).

An opt-in **`.ask --model`** mode using [Prem-1B-SQL](https://huggingface.co/premai-io/prem-1B-SQL) (MIT-licensed, 873 MB lazy download, ~51% on BirdBench) is planned for 0.1.8. That will handle the open-ended tail. The default `.ask` stays local, offline, deterministic — those are the properties that justify `.ask` being on by default.

## Extending the synonyms

Synonyms live in `src/ask/nl_to_sql.cpp`, function `ColumnSynonyms()`. Short and auditable by design — every entry is a place the engine guesses, which is also a place it can be wrong. Add entries via PR.

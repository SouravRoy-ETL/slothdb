# `.ask` - Natural-language queries in the SlothDB shell

<p align="center">
  <img src="../assets/ask-demo.svg" alt="slothdb .ask demo - natural-language queries translated to SQL" width="100%">
</p>

`.ask` turns plain natural language into SQL, prints the SQL, and runs it. The rules parser is English-only (hand-coded patterns); the local Qwen2.5-Coder fallback speaks 29 languages including English, Chinese, Spanish, French, German, Japanese, Korean, Russian, Arabic, Portuguese, Italian, and Hindi. **Nothing leaves the machine.** No API keys, no tokens on the wire, no schema leakage - the whole pipeline is in the same MIT binary SlothDB already ships. Set `SLOTHDB_ASK_CONFIRM=1` if you want a `[Y/n]` prompt before every run.

## How it works

Three layers, cheapest first. The router decides which one serves each question:

1. **Rules parser** (always on, instant, ~50 KB of C++). A pattern engine covering catalog introspection (`show tables`, `describe X`, `columns of X`), aggregates (COUNT / SUM / AVG / MIN / MAX), GROUP BY, TOP-N, year filters, latest/superlative, and file-source intents (`load sales.csv as sales`, `count rows in events.parquet`, `create view v from logs.json`). Sub-10 ms. No model touched.

2. **Local Qwen2.5-Coder-0.5B-Instruct-Q4_K_M** ([~310 MB GGUF](https://huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF), Apache 2.0). For open-ended questions the rules can't shape. ~200 ms on a laptop CPU via llama.cpp. Good at simple SELECT/GROUP BY/filter SQL.

3. **Local Qwen2.5-Coder-1.5B-Instruct-Q4_K_M** ([~986 MB GGUF](https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF), Apache 2.0). For analytic shapes: window functions, ranking within groups, LAG/LEAD, joins. Significantly better at `ROW_NUMBER() OVER (PARTITION BY ...)` style than the 0.5B.

**Parallel lazy download.** On first `.ask` invocation both tiers start downloading in detached threads. The 310 MB arrives in seconds and serves the first simple question immediately; the 986 MB keeps streaming in the background so the first analytic question doesn't pay a cold-download wait. Subsequent runs skip download (idempotent).

**Deterministic router.** `PickModelForQuestion(q)` scans the lowercase question for analytic signals - `per`, `within each`, `rank`, `most common`, `row_number`, `partition by`, `lag(`, `lead(`, `join `, `distinct`, `median`, `percentile`, `regression`, `top-`, `largest`, `highest`, etc. Match -> 1.5B. No match -> 0.5B. Pure function of the question; no LLM call involved in routing.

**Honest refusals.** Shapes the engine can't execute correctly (cumulative / running / moving / rolling aggregates) get a clean refusal instead of wrong numbers. SlothDB's window-frame executor has known gaps on unbounded-preceding frames; the rules parser catches those phrasings up front and tells you rather than asking Qwen to write SQL that returns the grand total on every row.

Zero cloud, zero API keys, zero telemetry. The only network activity is the one-time model download on first use.

## Two usage modes

**Single-shot:**

```
slothdb> .ask how many sales in 2024
-- Qwen2.5-Coder-0.5B-Instruct-Q4_K_M...
-- SELECT COUNT(*) FROM "sales" WHERE "order_date" LIKE '2024%'
Run? [Y/n] y
3
```

**Interactive** - type `.ask` alone. Keep asking without re-prefixing; `exit` / blank line / Ctrl-D leaves.

```
slothdb> .ask
ask mode: natural language -> SQL, inside the shell.
  Models (both lazy-downloaded in parallel on first use):
    small : Qwen2.5-Coder-0.5B-Instruct-Q4_K_M (~310 MB) -> simple COUNT / GROUP BY / filter / TOP-N
    large : Qwen2.5-Coder-1.5B-Instruct-Q4_K_M (~986 MB) -> window functions / joins / analytic
  Router picks per question. Rules-first handles catalog / simple shapes instantly.

ask> show tables
customers

ask> top 3 customers per region by revenue
-- Qwen2.5-Coder-1.5B-Instruct-Q4_K_M (rules did not match - asking the local model)
-- SELECT * FROM (SELECT region, customer_id, revenue,
--          ROW_NUMBER() OVER (PARTITION BY region ORDER BY revenue DESC) AS rn
--        FROM sales) WHERE rn <= 3
Run? [Y/n] y
...

ask> running total of revenue
  cumulative / running / moving / rolling aggregates need window-frame
  execution that SlothDB does not yet compute correctly. Rather than
  return wrong numbers, .ask refuses.

ask> exit
Back to SQL mode.
slothdb>
```

## Building with the embedded model

Default builds exclude the model - the binary stays small and there's no llama.cpp dependency. To turn it on:

```bash
git submodule update --init --depth 1 third_party/llama.cpp
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DSLOTHDB_ASK_MODEL=ON
cmake --build build --config Release
./build/src/Release/slothdb
```

First `.ask` call kicks off both-tier background downloads to `~/.slothdb/models/` (~310 MB + ~986 MB in parallel). The router blocks on whichever tier is needed for the current question; the other keeps streaming for next time. Subsequent runs are fully offline. Move or delete the files to pick up different models.

**Binary impact.** Default build stays small. With `SLOTHDB_ASK_MODEL=ON` the slothdb binary grows by ~30 MB (statically-linked llama.cpp + ggml). Model weights themselves are never bundled - they live on disk under `~/.slothdb/`.

## What the rules parser covers

When the model isn't compiled in, these phrasings still work:

| Shape | Example | Generates |
|---|---|---|
| Catalog | `show tables`, `list tables`, `what tables` | `SELECT table_name FROM information_schema.tables` |
| Count | `how many sales`, `count of sales`, `number of sales` | `COUNT(*)` |
| Count + year | `how many sales in 2024` | adds `WHERE date LIKE '2024%'` |
| Sum | `total amount`, `sum of amount` | `SUM(amount)` |
| Sum + group | `total amount per region` | adds `GROUP BY region` |
| Avg / Min / Max | `average amount`, `min amount`, `max amount` | `AVG` / `MIN` / `MAX` |
| Top-N | `top 5 customer_id by amount`, `bottom 3 region by amount` | `ORDER BY … DESC/ASC LIMIT N` |
| Latest / Oldest | `find me the latest sales`, `most recent sales`, `oldest sales` | `ORDER BY date_col DESC/ASC LIMIT 1` |
| Superlative | `which region had the most customer_id`, `which month had most orders` | `GROUP BY key` + `COUNT(DISTINCT)` or `SUM`, `ORDER BY … DESC LIMIT 1` |
| Select-all | `rows from sales` | `SELECT * FROM sales LIMIT 100` |

Year filtering (`in 2024`, `during 2024`) auto-detects a `date`-typed or `*_date` / `*_at` column. If there isn't one, the filter is skipped.

## Heuristics worth knowing

- **Singular ↔ plural**: `sale` resolves to `sales` (and back).
- **ID-column detection**: a metric named `id` or ending in `_id` is treated as an identifier, not a value. `which region had most customer_id` uses `COUNT(DISTINCT customer_id)`, not `SUM` - summing IDs is meaningless.
- **Exact match before synonym**: `total price` on a schema with `sales.amount` and `products.price` routes to `products`, not `sales`.
- **Filler words stripped**: `find`, `me`, `show`, `give`, `tell`, `list`, `which`, `who`, `had`, `has`, `please`, `all`, `the` - all stripped before matching.
- **Month / year / week / day grouping** on a typed `DATE` column uses `EXTRACT`; on a `VARCHAR` it groups by the whole string (SlothDB's planner currently rejects function expressions in `GROUP BY`).

## Column-name synonyms

When the NL noun doesn't match a column exactly, the rules parser consults a small synonym table. (The model ignores this and routes columns directly.)

| You say | Rules parser looks for |
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

Entries live in `src/ask/nl_to_sql.cpp`, function `ColumnSynonyms()`. Add via PR.

## Safety

Every generated SQL statement is printed before it runs, so you can catch wrong SQL by eye and `Ctrl-C` before results render. Default mode auto-runs - the session flow is *type question -> see SQL -> see result*. For a hard gate, set `SLOTHDB_ASK_CONFIRM=1` to restore the `[Y/n]` prompt between SQL and execution. The engine itself has no implicit-write path beyond the statement you just saw.

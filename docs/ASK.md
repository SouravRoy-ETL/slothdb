# `.ask` - Natural-language queries in the SlothDB shell

<p align="center">
  <img src="../assets/ask-demo.svg" alt="slothdb .ask demo - natural-language queries translated to SQL" width="100%">
</p>

`.ask` turns plain English into SQL, shows you the SQL, and prompts before running. Nothing leaves the machine - inference runs locally.

## How it works

Two layers, tried in this order:

1. **Embedded model** (when built with `-DSLOTHDB_ASK_MODEL=ON`). A ~310 MB [Qwen2.5-Coder-0.5B-Instruct](https://huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF) GGUF runs locally via llama.cpp. On the first query the model is downloaded to `~/.slothdb/models/`; subsequent queries are fully offline. Handles arbitrary NL.
2. **Rules parser** (always on). A ~50 KB pure-C++ pattern engine covering the common shapes (COUNT / SUM / AVG / MIN / MAX / GROUP BY / TOP-N / latest / superlative / `show tables`). Used as a fallback when the model isn't compiled in, or when the model can't produce SQL.

No cloud, no API keys, no telemetry. The only network activity is the one-time model download on first use, and only if `SLOTHDB_ASK_MODEL=ON` was set at build time.

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
ask mode - type a question in English. Model: Qwen2.5-Coder-0.5B-Instruct-Q4_K_M (loads on first query).
  `exit` / blank line / Ctrl+D to leave.

ask> show tables
-- SELECT table_name FROM information_schema.tables ORDER BY table_name
ask> most loyal repeat customers
-- SELECT "customer_id", COUNT(*) AS orders FROM "sales" GROUP BY "customer_id" HAVING COUNT(*) > 1 ORDER BY orders DESC LIMIT 10
Run? [Y/n] y
…
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

First `.ask` call downloads the GGUF (~310 MB) to `~/.slothdb/models/`. The model lives there permanently; move or delete the file to pick up a different model.

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

Every generated SQL statement is shown with a `[Y/n]` prompt before execution. There's no autorun, no implicit writes - the pipeline is always *see the SQL -> press y -> run it*.

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

Two AI fallbacks exist for the open-ended tail. Both are opt-in; **default `.ask` stays rules-only, local, deterministic**.

### `--ai` — cloud LLM (Ollama / OpenAI / Anthropic)

Works today if you already run one of these. Zero bytes added to SlothDB; the LLM is whatever you're running.

```bash
# Local Ollama (default)
ollama pull llama3:8b-instruct
SLOTHDB_ASK_PROVIDER=ollama slothdb

# OpenAI
export OPENAI_API_KEY=sk-...
SLOTHDB_ASK_PROVIDER=openai slothdb

# Anthropic
export ANTHROPIC_API_KEY=sk-ant-...
SLOTHDB_ASK_PROVIDER=anthropic slothdb
```

```
slothdb> .ask --ai most loyal repeat customers
-- trying ollama (llama3:8b-instruct)...
-- SELECT customer_id, COUNT(*) AS orders FROM sales GROUP BY customer_id HAVING COUNT(*) > 1 ORDER BY orders DESC LIMIT 10
Run? [Y/n]
```

Env vars:
- `SLOTHDB_ASK_PROVIDER` — `ollama` (default) / `openai` / `anthropic` / `none`
- `SLOTHDB_ASK_MODEL` — model name (e.g. `llama3:8b`, `gpt-4o-mini`, `claude-3-5-haiku-20241022`)
- `SLOTHDB_ASK_HOST` — for Ollama (e.g. `127.0.0.1:11434`)
- `OPENAI_API_KEY` / `ANTHROPIC_API_KEY` — for the respective providers

The core SlothDB engine stays offline — only `.ask --ai` ever makes an outbound call, and only with an explicit flag or `:ai` toggle.

### <a id="embedded-model"></a>`--model` — embedded local GGUF (opt-in build)

Fully local, no network, no API keys. Enabled at build time; the model file is lazy-downloaded to `~/.slothdb/models/` on first use.

```bash
# One-time setup:
git submodule update --init --depth 1 third_party/llama.cpp
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DSLOTHDB_ASK_MODEL=ON
cmake --build build --config Release
./build/src/slothdb

# In the shell — first call downloads ~310 MB; subsequent calls are offline:
slothdb> .ask --model most loyal repeat customers
-- running local Qwen2.5-Coder-0.5B-Instruct-Q4_K_M...
  Downloading Qwen2.5-Coder-0.5B-Instruct-Q4_K_M (~310 MB) from HuggingFace...
  (one-time; lives in ~/.slothdb/models/)
-- SELECT "customer_id", COUNT(*) AS orders FROM "sales" GROUP BY "customer_id" HAVING COUNT(*) > 1 ORDER BY orders DESC LIMIT 10
Run? [Y/n]
```

**Model choice.** Pinned to [Qwen2.5-Coder-0.5B-Instruct at Q4_K_M](https://huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF) — Apache 2.0, ~310 MB on disk, coding-tuned (better than the generic Instruct variant on SQL tasks for the same size). A future release may swap to a SlothDB-specific fine-tune at smaller size.

**Binary impact.** Default build stays at 8 MB — the `SLOTHDB_ASK_MODEL` option is off by default and `llama.cpp` is only compiled when the flag is on. The model weights themselves are never bundled in the binary; they live in `~/.slothdb/models/` and the user controls when they download.

**Status (0.1.7):** Scaffolding landed. Full inference path is wired but marked experimental — the llama.cpp API changes across releases and we haven't stress-tested against every Windows/Linux/macOS toolchain. Contributions welcome. If the build fails for you, report with the CMake error and the llama.cpp commit hash from `git -C third_party/llama.cpp log -1`.

### Interactive mode flags

Inside the `ask>` sub-REPL you can switch backend per-line or stickily:

```
ask> .ai                     # sticky: route all future lines via --ai
ask> .model                  # sticky: route via --model
ask> .rules                  # sticky: rules-only (default; refuses on miss)
ask> most loyal customers --ai     # one-off override on this line
```

## Extending the synonyms

Synonyms live in `src/ask/nl_to_sql.cpp`, function `ColumnSynonyms()`. Short and auditable by design — every entry is a place the engine guesses, which is also a place it can be wrong. Add entries via PR.

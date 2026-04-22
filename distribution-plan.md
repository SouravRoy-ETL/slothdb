# SlothDB distribution plan

_Private notes — not part of the public site. Use as a to-do list for when the blog post is live._

Context: Reddit is off the table (account banned). No HN karma yet. Fresh project, 8 stars. The [WASM playground](https://slothdb.org/playground/) is the shareable artifact. The [blog post](https://slothdb.org/blog/compiling-a-database-to-wasm.html) is the content vehicle — submit that URL, not the homepage.

Ordered by effort-to-payoff. One at a time, space out over 2-3 weeks.

## Tier 1 — high payoff, low risk (do first)

| Target | URL | Notes |
|---|---|---|
| **Console.dev** | https://console.dev/submit-tool/ | Explicitly solicits dev-tool tips. High inclusion rate for MIT C++ projects with playgrounds. |
| **DB Weekly** | https://dbweekly.com/ — submit via tip form at bottom of issue (Cooperpress newsletter) | Perfect audience fit. One-line pitch + playground link. |
| **Hacker Newsletter** | https://hackernewsletter.com/submit | 60k+ subs, curated by Kale Davis. Submit the blog post URL. |
| **Changelog News** | news@changelog.com (or https://changelog.com/news) | Covers new DBs (DuckDB, libSQL, TigerBeetle history). Lead with the blog post. |

## Tier 2 — needs good pitch but worth it

| Target | URL | Pitch angle |
|---|---|---|
| **TLDR Newsletter** | https://tldr.tech/tips | Short blurb. Lead with `1.1×–8.6× faster than DuckDB, now in browser` — numbers first. |
| **Pointer.io** | https://www.pointer.io/submit/ | Engineering-leadership audience. Submit the WASM-compilation essay, not a launch. |
| **Dev.to cross-post** | https://dev.to/new | Tags: `database`, `wasm`, `cpp`, `opensource`. Set `canonical_url` in frontmatter → slothdb.org. Wait 48h after slothdb.org publish so Google indexes the canonical first. |
| **Hashnode cross-post** | https://hashnode.com/ | Native canonical URL field. Same 48h delay. |
| **Bytes.dev** | Tips via reply to newsletter | JS-adjacent — lead with the WASM playground, not the DB. |

## Tier 3 — Lobsters (needs invite)

Lobsters has the best signal/noise of any submission site for systems content. The blog post fits tags `databases`, `c++`, `performance`, `wasm`, `practices`.

**Blocker:** posting requires invite from existing member. Paths to invite:
- Ask a DuckDB / DataFusion / Arrow contributor directly (many have invites)
- #lobsters IRC on Libera.Chat — explain the project honestly
- Hacker News co-submission — sometimes Lobsters users invite interesting submitters

Do NOT submit without an invite.

## Tier 4 — podcasts & video (high-variance, one shot each)

| Target | How | Odds |
|---|---|---|
| **Software Engineering Daily** | https://softwareengineeringdaily.com/contact/ | Has covered DuckDB, ClickHouse founders. Solo-maintainer WASM-DB is exactly their format. |
| **The Changelog podcast** | editors@changelog.com | OSS launches, DB-friendly. |
| **Developer Voices (Kris Jenkins)** | https://developervoices.com/ | Deep systems interviews. "Compiling a DB to WASM" is his exact format. |
| **ThePrimeagen** | Tweet the blog post at him | 50k views if he reacts. High variance, one shot. |
| **Fireship** | https://fireship.io/ contact | DB-to-WASM = his bait. Low odds but free. |

## Tier 5 — participate-not-promote communities

**Rule: answer questions, mention SlothDB only when it genuinely solves the question. No posts. No DMs.**

- **DuckDB Discord** (~10k) — https://discord.duckdb.org/ — friendly but moderated. Pitching "DuckDB alternative" reads as adversarial. Be helpful.
- **r/dataengineering Discord** (~40k) — https://discord.gg/dataengineering — project-sharing tolerated in #tools only.
- **dbt Community Slack** (70k+) — https://www.getdbt.com/community/join-the-community — #tools-and-integrations. Heavy mod on promo.
- **MLOps Community Slack** (25k+) — https://mlops.community/ — embedded DBs around feature stores.
- **Bytecode Alliance Zulip** — https://bytecodealliance.zulipchat.com/ — WASM-native audience.

## Twitter/X

Open an account. Post one tweet:

> SlothDB 0.1.5 runs in your browser now. 1,000-row GROUP BY in 3 ms, zero install, zero server. Full OLAP engine compiled to WebAssembly — here's what broke along the way:
>
> Playground: slothdb.org/playground
> Writeup: slothdb.org/blog/compiling-a-database-to-wasm.html

Pin the tweet. Don't reply-bait. Let the demo do the work.

## Sequencing (2 weeks)

**Week 1**
- Day 1 — Tier 1 submissions (Console, DB Weekly, Hacker Newsletter, Changelog News)
- Day 2 — Twitter post + pin
- Day 3 — TLDR + Pointer.io + Bytes.dev submissions
- Day 5 — Dev.to + Hashnode cross-posts with canonical URL

**Week 2**
- Day 8 — Podcast pitches (SED, Changelog, Developer Voices)
- Day 9 — ThePrimeagen/Fireship tweet
- Day 10+ — Join the five communities, lurk for a week, then start answering questions

**Week 3+**
- Chase a Lobsters invite
- Measure: GitHub stars, playground visits (slothdb.org analytics), newsletter mentions

## What not to do

- No Reddit. Account is banned; a new account posting similar content within a month triggers sitewide action that can blacklist the project.
- No Show HN submission until slothdb.org/blog traffic proves the post ranks. Submit the blog URL, not the project homepage, when you do submit.
- No mass DMs. Ever.
- No paying for newsletter placement. The free tiers are 90% of the reach.

## Verification note

Some links in this plan come from training-data recall rather than live verification. Before pitching each outlet, spend 30 seconds confirming the submission URL / editor email is current. Mark dead links as they appear.

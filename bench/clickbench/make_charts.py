"""Generate ClickBench-43 benchmark charts from a verify run.

Reads the table written by verify_all.py and renders two PNGs into
docs/assets/benchmarks/:
  clickbench_outcomes.png  - outcome breakdown across the 43 queries
  clickbench_speedup.png   - per-query speedup vs DuckDB

Numbers are exactly what the verify run measured. Queries SlothDB loses
are shown, not hidden.
"""
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "_private/orchestrator/phase2_clickbench43_verify.md"
OUT = ROOT / "docs/assets/benchmarks"
OUT.mkdir(parents=True, exist_ok=True)

GREEN = "#2e9e5b"
TEAL = "#4bb5a0"
RED = "#d4543f"
GREY = "#9aa0a6"
INK = "#1d2021"
HW = "ClickBench hits.parquet, 100M rows  .  6-core laptop  .  warm cache, min-of-3"


def parse(md_path):
    rows = []
    for line in md_path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 5 or not cells[0].isdigit():
            continue
        num = int(cells[0])
        status = cells[1]
        sloth = re.sub(r"[^\d.]", "", cells[2]) or None
        duck = re.sub(r"[^\d.]", "", cells[3]) or None
        ratio = re.sub(r"[^\d.]", "", cells[4]) or None
        rows.append({
            "q": num,
            "status": status,
            "sloth": float(sloth) if sloth else None,
            "duck": float(duck) if duck else None,
            "ratio": float(ratio) if ratio else None,
        })
    return rows


def outcomes_chart(rows):
    win = sum(r["status"] == "WIN" for r in rows)
    win_df = sum(r["status"] == "WIN_DF" for r in rows)
    loss = sum(r["status"] == "LOSS" for r in rows)
    timeout = sum(r["status"] in ("TIMEOUT", "ERROR", "RUNTIME", "PARSE_ERROR")
                  for r in rows)

    labels = [
        f"SlothDB faster  ({win})",
        f"DuckDB rejects the query, SlothDB runs it  ({win_df})",
        f"DuckDB faster  ({loss})",
        f"SlothDB over 30s  ({timeout})",
    ]
    sizes = [win, win_df, loss, timeout]
    colors = [GREEN, TEAL, RED, GREY]

    fig, ax = plt.subplots(figsize=(8.4, 5.4))
    wedges, _ = ax.pie(sizes, colors=colors, startangle=90,
                       counterclock=False,
                       wedgeprops=dict(width=0.42, edgecolor="white",
                                       linewidth=2))
    ax.text(0, 0.08, f"{win + win_df}/43", ha="center", va="center",
            fontsize=34, fontweight="bold", color=INK)
    ax.text(0, -0.16, "SlothDB ahead", ha="center", va="center",
            fontsize=12, color=GREY)
    ax.legend(wedges, labels, loc="center", bbox_to_anchor=(0.5, -0.13),
              ncol=2, frameon=False, fontsize=9.5)
    ax.set_title("ClickBench-43: SlothDB vs DuckDB",
                 fontsize=15, fontweight="bold", color=INK, pad=14)
    fig.text(0.5, 0.045, HW, ha="center", fontsize=8, color=GREY)
    fig.savefig(OUT / "clickbench_outcomes.png", dpi=150,
                bbox_inches="tight", facecolor="white")
    plt.close(fig)
    return win, win_df, loss, timeout


def speedup_chart(rows):
    comp = [r for r in rows if r["status"] in ("WIN", "LOSS") and r["ratio"]]
    comp.sort(key=lambda r: r["ratio"], reverse=True)
    labels = [f"Q{r['q']}" for r in comp]
    ratios = [r["ratio"] for r in comp]
    colors = [GREEN if x >= 1.0 else RED for x in ratios]

    fig, ax = plt.subplots(figsize=(13, 5))
    ax.bar(range(len(ratios)), ratios, color=colors, width=0.78)
    ax.axhline(1.0, color=INK, linewidth=1, linestyle="--")
    ax.set_yscale("log")
    ax.set_yticks([0.25, 0.5, 1, 2, 4, 8])
    ax.set_yticklabels(["0.25x", "0.5x", "1x", "2x", "4x", "8x"])
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, rotation=90, fontsize=8)
    ax.set_ylabel("Speedup vs DuckDB  (higher is better)", fontsize=10)
    ax.set_xlim(-0.7, len(ratios) - 0.3)
    nwin = sum(x >= 1.0 for x in ratios)
    ax.set_title(f"Per-query speedup, {len(ratios)} head-to-head queries: "
                 f"SlothDB faster on {nwin}, DuckDB faster on "
                 f"{len(ratios) - nwin}",
                 fontsize=13, fontweight="bold", color=INK, pad=10)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    fig.text(0.5, -0.02, HW, ha="center", fontsize=8, color=GREY)
    fig.savefig(OUT / "clickbench_speedup.png", dpi=150,
                bbox_inches="tight", facecolor="white")
    plt.close(fig)


def main():
    if not SRC.exists():
        sys.exit(f"verify table not found: {SRC}")
    rows = parse(SRC)
    if len(rows) != 43:
        print(f"warning: parsed {len(rows)} rows, expected 43")
    win, win_df, loss, timeout = outcomes_chart(rows)
    speedup_chart(rows)
    print(f"parsed {len(rows)} queries")
    print(f"  SlothDB faster      {win}")
    print(f"  DuckDB rejects, run {win_df}")
    print(f"  DuckDB faster       {loss}")
    print(f"  timeout             {timeout}")
    print(f"wrote {OUT}/clickbench_outcomes.png")
    print(f"wrote {OUT}/clickbench_speedup.png")


if __name__ == "__main__":
    main()

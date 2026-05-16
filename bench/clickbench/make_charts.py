"""Render the ClickBench-43 charts for the README and slothdb.org.

Reads clickbench_results.csv (one row per query, committed alongside this
script so the charts are reproducible) and writes four PNGs into
docs/assets/benchmarks/: a dark and a light variant of the outcome donut
and the per-query speedup chart. The palette matches the slothdb.org
theme. Queries SlothDB loses are drawn, not dropped.
"""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve()
CSV = HERE.parent / "clickbench_results.csv"
OUT = HERE.parents[2] / "docs/assets/benchmarks"
OUT.mkdir(parents=True, exist_ok=True)

plt.rcParams["font.sans-serif"] = ["Segoe UI", "Inter", "Helvetica Neue",
                                   "Arial", "DejaVu Sans"]
plt.rcParams["font.family"] = "sans-serif"

CAPTION = ("43 queries  .  100M-row hits.parquet  .  6-core laptop "
           "(Ryzen 5 5600U)  .  warm cache, min-of-3")

# Palette lifted from docs/style.css (dark default, light toggle).
THEMES = {
    "": dict(bg="#0E0E10", text="#FAFAFA", dim="#9C9CA7", muted="#6B6B77",
             win="#34D399", accent="#B794F4", danger="#F87171",
             axis="#2A2A31"),
    "_light": dict(bg="#FFFFFF", text="#0A0A0B", dim="#4A4A55",
                   muted="#7B7B85", win="#059669", accent="#7C3AED",
                   danger="#DC2626", axis="#D7D7DD"),
}


def load():
    rows = []
    with open(CSV, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            sloth = float(r["sloth_ms"]) if r["sloth_ms"] else None
            duck = float(r["duck_ms"]) if r["duck_ms"] else None
            rows.append({"q": int(r["q"]), "status": r["status"],
                         "sloth": sloth, "duck": duck})
    return rows


def donut(rows, th, suffix):
    win = sum(r["status"] == "WIN" for r in rows)
    win_df = sum(r["status"] == "WIN_DF" for r in rows)
    loss = sum(r["status"] == "LOSS" for r in rows)
    timeout = sum(r["status"] not in ("WIN", "WIN_DF", "LOSS") for r in rows)

    vals = [win, win_df, loss, timeout]
    colors = [th["win"], th["accent"], th["danger"], th["muted"]]
    labels = [f"SlothDB faster   {win}",
              f"DuckDB rejects the query   {win_df}",
              f"DuckDB faster   {loss}",
              f"SlothDB over 30s   {timeout}"]

    fig, ax = plt.subplots(figsize=(8.2, 5.8))
    fig.patch.set_facecolor(th["bg"])
    wedges, _ = ax.pie(vals, colors=colors, startangle=90, counterclock=False,
                       wedgeprops=dict(width=0.40, edgecolor=th["bg"],
                                       linewidth=3.5))
    ax.text(0, 0.12, f"{win + win_df}/43", ha="center", va="center",
            fontsize=44, fontweight="bold", color=th["text"])
    ax.text(0, -0.20, "ahead of DuckDB", ha="center", va="center",
            fontsize=12, color=th["dim"])
    ax.set_title("ClickBench-43: SlothDB vs DuckDB", fontsize=16,
                 fontweight="bold", color=th["text"], pad=18)
    leg = ax.legend(wedges, labels, loc="center", bbox_to_anchor=(0.5, -0.12),
                    ncol=2, frameon=False, fontsize=10, labelcolor=th["dim"],
                    handlelength=1.1, columnspacing=2.4, handletextpad=0.7)
    fig.text(0.5, 0.05, CAPTION, ha="center", fontsize=8, color=th["muted"])
    fig.savefig(OUT / f"clickbench_outcomes{suffix}.png", dpi=160,
                bbox_inches="tight", facecolor=th["bg"])
    plt.close(fig)


def speedup(rows, th, suffix):
    comp = [r for r in rows if r["status"] in ("WIN", "LOSS")
            and r["sloth"] and r["duck"]]
    comp.sort(key=lambda r: r["duck"] / r["sloth"], reverse=True)
    ratios = [r["duck"] / r["sloth"] for r in comp]
    labels = [f"Q{r['q']}" for r in comp]
    colors = [th["win"] if x >= 1.0 else th["danger"] for x in ratios]
    nwin = sum(x >= 1.0 for x in ratios)

    fig, ax = plt.subplots(figsize=(13, 4.9))
    fig.patch.set_facecolor(th["bg"])
    ax.set_facecolor(th["bg"])
    ax.bar(range(len(ratios)), ratios, color=colors, width=0.74)
    ax.axhline(1.0, color=th["dim"], linewidth=1, linestyle=(0, (4, 3)))
    ax.set_yscale("log")
    ax.set_yticks([0.25, 0.5, 1, 2, 4, 8])
    ax.set_yticklabels(["0.25x", "0.5x", "1x", "2x", "4x", "8x"])
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, rotation=90, fontsize=8, color=th["dim"])
    ax.set_xlim(-0.7, len(ratios) - 0.3)
    ax.tick_params(axis="y", colors=th["dim"], labelsize=9)
    ax.tick_params(axis="x", length=0)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(th["axis"])
    ax.set_title(f"Per-query speedup vs DuckDB    SlothDB faster on {nwin} "
                 f"of {len(ratios)} head-to-head queries", fontsize=13,
                 fontweight="bold", color=th["text"], pad=12)
    fig.text(0.5, -0.04, CAPTION, ha="center", fontsize=8, color=th["muted"])
    fig.savefig(OUT / f"clickbench_speedup{suffix}.png", dpi=160,
                bbox_inches="tight", facecolor=th["bg"])
    plt.close(fig)


def main():
    rows = load()
    for suffix, th in THEMES.items():
        donut(rows, th, suffix)
        speedup(rows, th, suffix)
    print(f"{len(rows)} queries -> {OUT}")
    for suffix in THEMES:
        print(f"  clickbench_outcomes{suffix}.png  clickbench_speedup{suffix}.png")


if __name__ == "__main__":
    main()

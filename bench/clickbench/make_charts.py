"""Render the honest ClickBench-43 charts for the README and slothdb.org.

Reads official_results.csv (written by official_bench.py: one row per query,
columns q, sloth_ms, duck_ms, result) and writes a dark and a light variant
of two charts into docs/assets/benchmarks/: an outcome donut and the
per-query speedup chart. Both engines are timed head to head on the same
machine; queries SlothDB loses or does not complete are drawn, not dropped.

  python make_charts.py [official_results.csv] [out_dir]
"""
import csv
import math
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve()
CSV = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "official_results.csv"
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE.parents[2] / "docs/assets/benchmarks"
OUT.mkdir(parents=True, exist_ok=True)

plt.rcParams["font.sans-serif"] = ["Segoe UI", "Inter", "Helvetica Neue",
                                   "Arial", "DejaVu Sans"]
plt.rcParams["font.family"] = "sans-serif"

CAPTION = ("43 official ClickBench queries  .  ~100M-row hits dataset  .  "
           "SlothDB vs DuckDB on the same machine  .  3 trials, min reported")

# Palette lifted from docs/style.css (dark default, light toggle).
THEMES = {
    "": dict(bg="#0E0E10", text="#FAFAFA", dim="#9C9CA7", muted="#6B6B77",
             win="#34D399", danger="#F87171", axis="#2A2A31"),
    "_light": dict(bg="#FFFFFF", text="#0A0A0B", dim="#4A4A55", muted="#7B7B85",
                   win="#059669", danger="#DC2626", axis="#D7D7DD"),
}


def load():
    rows = []
    with open(CSV, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            sloth = float(r["sloth_ms"]) if r["sloth_ms"] else None
            duck = float(r["duck_ms"]) if r["duck_ms"] else None
            rows.append({"q": int(r["q"]), "sloth": sloth, "duck": duck,
                         "result": r["result"]})
    return rows


def comparable(rows):
    """Queries where both engines ran and produced a timed result."""
    return [r for r in rows if r["sloth"] and r["duck"]]


def geomean(comp):
    if not comp:
        return 0.0
    return math.exp(sum(math.log(r["duck"] / r["sloth"]) for r in comp)
                    / len(comp))


def donut(rows, th, suffix):
    comp = comparable(rows)
    faster = sum(r["duck"] > r["sloth"] for r in comp)
    slower = len(comp) - faster
    incomplete = sum(r["sloth"] is None for r in rows)

    vals = [faster, slower, incomplete]
    colors = [th["win"], th["danger"], th["muted"]]
    labels = [f"SlothDB faster   {faster}",
              f"DuckDB faster   {slower}",
              f"SlothDB did not finish   {incomplete}"]

    fig, ax = plt.subplots(figsize=(8.2, 5.8))
    fig.patch.set_facecolor(th["bg"])
    wedges, _ = ax.pie(vals, colors=colors, startangle=90, counterclock=False,
                       wedgeprops=dict(width=0.40, edgecolor=th["bg"],
                                       linewidth=3.5))
    ax.text(0, 0.12, f"{geomean(comp):.2f}x", ha="center", va="center",
            fontsize=44, fontweight="bold", color=th["text"])
    ax.text(0, -0.20, "geomean vs DuckDB", ha="center", va="center",
            fontsize=12, color=th["dim"])
    ax.set_title("ClickBench-43: SlothDB vs DuckDB", fontsize=16,
                 fontweight="bold", color=th["text"], pad=18)
    ax.legend(wedges, labels, loc="center", bbox_to_anchor=(0.5, -0.12),
              ncol=3, frameon=False, fontsize=10, labelcolor=th["dim"],
              handlelength=1.1, columnspacing=1.8, handletextpad=0.7)
    fig.text(0.5, 0.05, CAPTION, ha="center", fontsize=8, color=th["muted"])
    fig.savefig(OUT / f"clickbench_outcomes{suffix}.png", dpi=160,
                bbox_inches="tight", facecolor=th["bg"])
    plt.close(fig)


def speedup(rows, th, suffix):
    comp = comparable(rows)
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
    ax.set_yticks([0.125, 0.25, 0.5, 1, 2, 4, 8, 16])
    ax.set_yticklabels(["0.12x", "0.25x", "0.5x", "1x", "2x", "4x", "8x",
                        "16x"])
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
    comp = comparable(rows)
    print(f"{len(rows)} queries, {len(comp)} comparable, "
          f"geomean {geomean(comp):.2f}x -> {OUT}")


if __name__ == "__main__":
    main()

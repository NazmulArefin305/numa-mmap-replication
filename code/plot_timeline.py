#!/usr/bin/env python3
"""Plot the per-second timelines written by run_exp6.sh.

    python3 plot_timeline.py      # writes timeline_learn.png and timeline_move.png

Needs matplotlib (sudo apt install python3-matplotlib).
"""
import csv
import glob
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK, MUTED, GRID = "#1f1f1e", "#6b6a63", "#e4e3dd"
COLOURS = {"plain": "#eb6834", "known": "#1baf7a", "adaptive": "#2a78d6"}
LABELS = {"plain": "Plain mmap", "known": "Known hot set (fixed)",
          "adaptive": "Adaptive (counts reads)"}


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    return [float(r["t_s"]) for r in rows], [float(r["gib_per_s"]) for r in rows]


def move_time(path):
    """First second at which the hot set had moved (hot_base_block changed)."""
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for prev, cur in zip(rows, rows[1:]):
        if cur["hot_base_block"] != prev["hot_base_block"]:
            return float(cur["t_s"])
    return None


def style(ax, title):
    plt.rcParams.update({"font.size": 10})
    ax.set_title(title, loc="left", fontweight="bold", color=INK)
    ax.set_xlabel("Seconds since start", color=INK)
    ax.set_ylabel("Read throughput (GiB/s)", color=INK)
    ax.tick_params(colors=MUTED)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.grid(axis="y", color=GRID, lw=0.8)
    ax.set_axisbelow(True)
    ax.set_ylim(bottom=0)


def main():
    if os.path.exists("timeline_learn.csv"):
        t, g = load("timeline_learn.csv")
        fig, ax = plt.subplots(figsize=(7, 3.6))
        ax.plot(t, g, color=COLOURS["adaptive"], lw=2, marker="o", ms=4)
        style(ax, "Adaptive replicas from a cold start")
        fig.tight_layout()
        fig.savefig("timeline_learn.png", dpi=180, facecolor="white")
        print("Wrote timeline_learn.png")

    files = sorted(glob.glob("timeline_move_*.csv"))
    if files:
        fig, ax = plt.subplots(figsize=(8, 4))
        seen, moved = set(), None
        for path in files:
            moved = moved or move_time(path)
            setting = os.path.basename(path).split("_")[2]
            t, g = load(path)
            ax.plot(t, g, color=COLOURS.get(setting, MUTED), lw=1.8,
                    label=None if setting in seen else LABELS.get(setting, setting))
            seen.add(setting)
        style(ax, "Throughput per second; the hot set moves at the dashed line")
        if moved is not None:
            ax.axvline(moved, color=MUTED, lw=1, ls="--")
            ax.text(moved + 0.5, ax.get_ylim()[1] * 0.04, "hot set moves", color=MUTED, fontsize=9)
        ax.legend(frameon=False, fontsize=9, loc="lower right")
        fig.tight_layout()
        fig.savefig("timeline_move.png", dpi=180, facecolor="white")
        print("Wrote timeline_move.png")


if __name__ == "__main__":
    main()

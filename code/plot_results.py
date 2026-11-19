#!/usr/bin/env python3
"""Plot results.csv from run_experiment.sh and print a summary table.

    python3 plot_results.py results.csv        # writes results.png

Needs matplotlib (sudo apt install python3-matplotlib).
"""
import csv
import statistics
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# (mode, warm, numa_balancing) -> (label, colour, marker). The colour belongs
# to the setting, so a setting keeps its colour even if others are missing.
SERIES = {
    ("replica", "-", 0): ("Per-node replicas (this work)", "#2a78d6", "o"),
    ("shared", "spread", 0): ("mmap, pages spread over nodes", "#eb6834", "s"),
    ("shared", "spread", 1): ("mmap, spread + NUMA balancing", "#1baf7a", "^"),
    ("shared", "single", 0): ("mmap, all pages on one node", "#eda100", "D"),
}
INK, MUTED, GRID = "#1f1f1e", "#6b6a63", "#e4e3dd"


def load(path):
    thr = defaultdict(lambda: defaultdict(list))   # key -> threads -> [GiB/s]
    loc = defaultdict(list)                        # key -> [local ratio]
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            nb = int(row["numa_balancing"])
            key = (row["mode"], row["warm"], 1 if nb > 0 else 0)
            if key not in SERIES:
                continue
            thr[key][int(row["threads"])].append(float(row["gib_per_s"]))
            if float(row["local_after"]) >= 0:
                loc[key].append(float(row["local_after"]))
    return thr, loc


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "results.csv"
    out = sys.argv[2] if len(sys.argv) > 2 else path.rsplit(".", 1)[0] + ".png"
    thr, loc = load(path)
    if not thr:
        sys.exit(f"No usable rows in {path}")
    keys = [k for k in SERIES if k in thr]

    plt.rcParams.update({"font.size": 10, "text.color": INK, "axes.labelcolor": INK,
                         "xtick.color": MUTED, "ytick.color": MUTED,
                         "axes.edgecolor": GRID})
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.4),
                                   gridspec_kw={"width_ratios": [3, 2]})

    # Left: throughput against thread count (mean, with min-max whiskers)
    for key in keys:
        label, colour, marker = SERIES[key]
        ts = sorted(thr[key])
        means = [statistics.mean(thr[key][t]) for t in ts]
        lo = [m - min(thr[key][t]) for m, t in zip(means, ts)]
        hi = [max(thr[key][t]) - m for m, t in zip(means, ts)]
        ax1.errorbar(ts, means, yerr=[lo, hi], label=label, color=colour, marker=marker,
                     markersize=6, linewidth=2, capsize=3, markeredgecolor="white",
                     markeredgewidth=1)
    ax1.set_xscale("log", base=2)
    all_t = sorted({t for k in keys for t in thr[k]})
    ax1.set_xticks(all_t)
    ax1.set_xticklabels([str(t) for t in all_t])
    ax1.minorticks_off()
    ax1.set_ylim(bottom=0)
    ax1.set_xlabel("Reader threads (spread evenly over NUMA nodes)")
    ax1.set_ylabel("Read throughput (GiB/s)")
    ax1.set_title("Random 4 KiB reads from a mapped file", loc="left", fontweight="bold")
    ax1.legend(frameon=False, fontsize=9)

    # Right: share of accesses that were local
    lkeys = [k for k in keys if loc[k]]
    if lkeys:
        vals = [100 * statistics.mean(loc[k]) for k in lkeys]
        ypos = list(range(len(lkeys)))[::-1]
        ax2.barh(ypos, vals, height=0.55, color=[SERIES[k][1] for k in lkeys])
        for y, v in zip(ypos, vals):
            ax2.text(min(v + 2, 88), y, f"{v:.0f}%", va="center", color=INK,
                     fontsize=9, bbox=dict(facecolor="white", edgecolor="none", pad=1)
                     if v + 2 > 88 else None)
        ax2.set_yticks(ypos)
        ax2.set_yticklabels([SERIES[k][0].replace(" (this work)", "") for k in lkeys],
                            color=INK, fontsize=9)
        ax2.set_xlim(0, 100)
        ax2.set_xlabel("Accesses served from the local node (%)")
    else:
        ax2.text(0.5, 0.5, "Locality not measured\n(needs a real NUMA machine)",
                 ha="center", va="center", color=MUTED, transform=ax2.transAxes)
        ax2.set_xticks([])
        ax2.set_yticks([])
    ax2.set_title("Memory locality", loc="left", fontweight="bold")

    for ax in (ax1, ax2):
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.grid(axis="y" if ax is ax1 else "x", color=GRID, linewidth=0.8)
        ax.set_axisbelow(True)
    fig.tight_layout()
    fig.savefig(out, dpi=160, facecolor="white")
    print(f"Wrote {out}\n")

    # Summary table: speed-up of the replicas over the best baseline
    rep = ("replica", "-", 0)
    base_keys = [k for k in keys if k != rep]
    print(f"{'threads':>7}  {'replicas':>9}  {'best mmap':>9}  {'speed-up':>8}   (GiB/s, mean)")
    for t in all_t:
        if rep not in thr or t not in thr[rep]:
            continue
        r = statistics.mean(thr[rep][t])
        bases = [statistics.mean(thr[k][t]) for k in base_keys if t in thr[k]]
        if bases:
            print(f"{t:>7}  {r:>9.2f}  {max(bases):>9.2f}  {r / max(bases):>7.2f}x")


if __name__ == "__main__":
    main()

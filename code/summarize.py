#!/usr/bin/env python3
"""Print a summary table of any results CSV made by numa_mmap_bench.

    python3 summarize.py results_exp3.csv

Rows with the same settings are averaged. For experiment 4 files (rows with
read shares below 100%) it also prints the replica speed-up per read share.
Needs only the Python standard library.
"""
import csv
import statistics
import sys
from collections import defaultdict


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: python3 summarize.py RESULTS.csv")
    rows = list(csv.DictReader(open(sys.argv[1], newline="")))
    if not rows:
        sys.exit("no rows")

    groups = defaultdict(list)
    for r in rows:
        key = (r["mode"], r.get("warm", "-"), r.get("numa_balancing", "?"),
               r.get("pinned", "yes"), r.get("read_pct", "100"),
               r.get("writable", "0"), int(r["threads"]))
        groups[key].append(r)

    def mean(rs, col):
        vals = [num(x.get(col)) for x in rs]
        vals = [v for v in vals if v is not None and v >= 0]
        return statistics.mean(vals) if vals else None

    def fmt(v, pct=False, digits=2):
        if v is None:
            return "-"
        return f"{100 * v:.1f}%" if pct else f"{v:.{digits}f}"

    head = ["mode", "pinned", "NB", "reads", "threads", "runs", "GiB/s",
            "M ops/s", "local", "node chg/s", "M refresh/s", "errors"]
    print("  ".join(f"{h:>11}" for h in head))
    for key in sorted(groups, key=lambda k: (k[0], k[3], k[2], -int(k[4]), k[6])):
        rs = groups[key]
        local = mean(rs, "sampled_local")
        if local is None:
            local = mean(rs, "local_after")
        errors = sum(int(num(x.get("check_errors")) or 0) for x in rs)
        cells = [key[0], key[3], key[2], key[4] + "%", str(key[6]), str(len(rs)),
                 fmt(mean(rs, "gib_per_s")), fmt(mean(rs, "mops_per_s")),
                 fmt(local, pct=True), fmt(mean(rs, "node_changes_per_thread_s")),
                 fmt(mean(rs, "refreshes_per_s"), digits=3), str(errors)]
        print("  ".join(f"{c:>11}" for c in cells))

    # Experiment 5: settings under each memory limit
    if any((num(r.get("mem_limit_gib")) or -1) > 0 for r in rows):
        def setting(r):
            if r["mode"] == "shared":
                return "plain"
            f = num(r.get("rep_frac")) or 0
            return "full" if f > 0.99 else f"hot{round(100 * f)}"
        by = defaultdict(list)
        for r in rows:
            by[(round(num(r["mem_limit_gib"]) or -1), setting(r))].append(r)
        names = ["plain", "full", "hot10", "hot25"]
        print("\nThroughput (GiB/s), speed-up vs plain mmap, and disk reads (MB/s) per memory limit:")
        print("  limit  " + "  ".join(f"{n:>22}" for n in names))
        for lim in sorted({k[0] for k in by}, reverse=True):
            base = by.get((lim, "plain"))
            b = mean(base, "gib_per_s") if base else None
            cells = []
            for n in names:
                rs = by.get((lim, n))
                if not rs:
                    cells.append(f"{'-':>22}")
                    continue
                g, dsk = mean(rs, "gib_per_s"), mean(rs, "disk_read_mb_s") or 0.0
                su = f"{g / b:.2f}x" if b else "-"
                cells.append(f"{g:7.2f} {su:>6} {dsk:6.0f}MB/s")
            print(f"  {lim:>3} GiB  " + "  ".join(cells))

    # Experiment 4: speed-up of replicas over plain mmap, per read share
    if any(r.get("read_pct", "100") != "100" for r in rows):
        by = defaultdict(list)
        for r in rows:
            by[(r["mode"], int(r["read_pct"]), int(r["threads"]))].append(num(r["mops_per_s"]))
        shares = sorted({k[1] for k in by}, reverse=True)
        threads = sorted({k[2] for k in by})
        print("\nReplica speed-up over plain mmap (operations per second):")
        print("  reads  " + "  ".join(f"{t:>3} threads" for t in threads))
        for p in shares:
            cells = []
            for t in threads:
                s, rp = by.get(("shared", p, t)), by.get(("replica", p, t))
                cells.append(f"{statistics.mean(rp) / statistics.mean(s):>10.2f}x"
                             if s and rp else f"{'-':>11}")
            print(f"  {p:>4}%  " + "  ".join(cells))


if __name__ == "__main__":
    main()

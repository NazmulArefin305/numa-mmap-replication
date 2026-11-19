#!/usr/bin/env bash
# Experiment 1: does Linux NUMA balancing look at a memory-mapped file at all?
#
#   sudo ./run_exp1.sh          (about 5 minutes)
#
#   1a  read-only mapping of the file      (repeat of the earlier manual run)
#   1b  the same file mapped read-write    (never written)
#   1c  positive control: anonymous memory placed on node 0, read only by
#       threads on node 1, first with NUMA balancing off, then on
#
# For each run it saves the change in the kernel's NUMA balancing counters
# (vmstat_1a.txt, vmstat_1b.txt, vmstat_1c.txt) and adds the benchmark result
# to results_exp1.csv.
#
# Settings: FILE (default ../numa-mmap-replica/data.bin), SECS (default 60).
set -euo pipefail
cd "$(dirname "$0")"

FILE=${FILE:-../numa-mmap-replica/data.bin}
SECS=${SECS:-60}
OUT=results_exp1.csv
NB=/proc/sys/kernel/numa_balancing

[ -x ./numa_mmap_bench ] || make
[ -f "$FILE" ] || { echo "Data file $FILE not found. Set FILE=..." >&2; exit 1; }
[ -w "$NB" ] || { echo "Run this with sudo (it switches NUMA balancing on and off)." >&2; exit 1; }

nb_orig=$(cat "$NB")
trap 'echo "$nb_orig" > "$NB"' EXIT

snap() { grep -E '^numa_(pte_updates|hint_faults|hint_faults_local|pages_migrated) ' /proc/vmstat; }

# measure NAME command...  -> runs the command, saves the counter changes
measure() {
    local name=$1; shift
    snap > .before
    "$@" >> "$OUT"
    snap > .after
    paste .before .after | awk '{print $1, $4 - $2}' > "vmstat_$name.txt"
    rm -f .before .after
    echo "  counter changes ($name):"
    sed 's/^/    /' "vmstat_$name.txt"
}

./numa_mmap_bench -H > "$OUT"
sync; echo 3 > /proc/sys/vm/drop_caches

echo 1 > "$NB"
echo "== 1a: read-only mapping, NUMA balancing on, 40 threads, $SECS s"
measure 1a ./numa_mmap_bench -f "$FILE" -m shared -t 40 -d "$SECS" -e

echo "== 1b: same file mapped read-write (no writes), NUMA balancing on"
measure 1b ./numa_mmap_bench -f "$FILE" -m shared -W -t 40 -d "$SECS" -e

echo 0 > "$NB"
echo "== 1c (control, balancing OFF): anonymous memory on node 0, 20 threads on node 1"
./numa_mmap_bench -f "$FILE" -m anon -n 1 -t 20 -d "$SECS" >> "$OUT"

echo 1 > "$NB"
echo "== 1c (control, balancing ON): the same"
measure 1c ./numa_mmap_bench -f "$FILE" -m anon -n 1 -t 20 -d "$SECS"

echo
echo "Done. Counter files: vmstat_1a.txt vmstat_1b.txt vmstat_1c.txt; results: $OUT"
echo "Summary: python3 summarize.py $OUT"

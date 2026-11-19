#!/usr/bin/env bash
# Experiment 3: threads that are free to move between sockets.
#
#   sudo ./run_exp3.sh          (about 20 minutes)
#
# Four settings, each at THREADS thread counts, REPS times, SECS seconds:
#   mmap, unpinned, NUMA balancing off
#   mmap, unpinned, NUMA balancing on
#   replicas, unpinned (each thread uses the copy on the socket it runs on)
#   replicas, pinned  (reference, as in run 1)
# Locality is measured by sampling, which works for unpinned threads.
#
# Settings: FILE (default ../numa-mmap-replica/data.bin), THREADS (8 20 40),
# REPS (3), SECS (20), OUT (results_exp3.csv).
set -euo pipefail
cd "$(dirname "$0")"

FILE=${FILE:-../numa-mmap-replica/data.bin}
THREADS=${THREADS:-"8 20 40"}
REPS=${REPS:-3}
SECS=${SECS:-20}
OUT=${OUT:-results_exp3.csv}
NB=/proc/sys/kernel/numa_balancing

[ -x ./numa_mmap_bench ] || make
[ -f "$FILE" ] || { echo "Data file $FILE not found. Set FILE=..." >&2; exit 1; }
[ -w "$NB" ] || { echo "Run this with sudo (it switches NUMA balancing on and off)." >&2; exit 1; }

nb_orig=$(cat "$NB")
trap 'echo "$nb_orig" > "$NB"' EXIT

./numa_mmap_bench -H > "$OUT"

# run_config LABEL BALANCING benchmark-options...
run_config() {
    local label=$1 nb=$2; shift 2
    echo "== $label (NUMA balancing $nb)"
    echo "$nb" > "$NB"
    sync; echo 3 > /proc/sys/vm/drop_caches
    local first=1
    for t in $THREADS; do
        for _ in $(seq "$REPS"); do
            local e=()
            [ "$first" = 1 ] && e=(-e)
            first=0
            ./numa_mmap_bench -f "$FILE" "$@" -t "$t" -d "$SECS" "${e[@]}" >> "$OUT"
        done
    done
}

run_config "mmap, unpinned" 0 -m shared -u
run_config "mmap, unpinned" 1 -m shared -u
run_config "replicas, unpinned" 0 -m replica -u
run_config "replicas, pinned" 0 -m replica

echo
echo "Done. Results: $OUT   Summary: python3 summarize.py $OUT"

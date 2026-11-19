#!/usr/bin/env bash
# Experiment 4: reads and writes, with PaCaR's main/twin rule.
#
#   sudo ./run_exp4.sh          (about 35 minutes)
#
# 1. Correctness check: replicas with 50% writes and -C; must report 0 errors.
# 2. For each read share in READS and thread count in THREADS, REPS times:
#      plain mmap with the same mix            (baseline)
#      replicas with the main/twin protocol    (-P: protocol on even at 100%)
# 3. The cost of catching a write with a page fault (fault_cost), with 0, 19
#    and 39 extra threads running.
#
# WARNING: the runs with writes change the contents of the data file.
#
# Settings: FILE (default ../numa-mmap-replica/data.bin), READS
# ("100 99 95 90 75 65 50"), THREADS ("16 40"), REPS (3), SECS (20).
set -euo pipefail
cd "$(dirname "$0")"

FILE=${FILE:-../numa-mmap-replica/data.bin}
READS=${READS:-"100 99 95 90 75 65 50"}
THREADS=${THREADS:-"16 40"}
REPS=${REPS:-3}
SECS=${SECS:-20}
OUT=${OUT:-results_exp4.csv}
NB=/proc/sys/kernel/numa_balancing

[ -x ./numa_mmap_bench ] && [ -x ./fault_cost ] || make
[ -f "$FILE" ] || { echo "Data file $FILE not found. Set FILE=..." >&2; exit 1; }
[ -w "$NB" ] || { echo "Run this with sudo (it switches NUMA balancing off)." >&2; exit 1; }

nb_orig=$(cat "$NB")
trap 'echo "$nb_orig" > "$NB"' EXIT
echo 0 > "$NB"

echo "== Correctness check: replicas, 50% writes, 40 threads, 10 s"
./numa_mmap_bench -H > results_exp4_check.csv
./numa_mmap_bench -f "$FILE" -m replica -r 50 -C -t 40 -d 10 >> results_exp4_check.csv
errors=$(tail -1 results_exp4_check.csv | awk -F, '{print $NF}')
if [ "$errors" != "0" ]; then
    echo "STOP: the consistency check found $errors torn reads. Send this output to Claude." >&2
    exit 1
fi
echo "   0 torn reads: the protocol is consistent."

./numa_mmap_bench -H > "$OUT"
sync; echo 3 > /proc/sys/vm/drop_caches
first=1
for r in $READS; do
    echo "== $r% reads"
    for t in $THREADS; do
        for _ in $(seq "$REPS"); do
            e=()
            [ "$first" = 1 ] && e=(-e)
            first=0
            ./numa_mmap_bench -f "$FILE" -m shared -r "$r" -t "$t" -d "$SECS" "${e[@]}" >> "$OUT"
            ./numa_mmap_bench -f "$FILE" -m replica -P -r "$r" -t "$t" -d "$SECS" >> "$OUT"
        done
    done
done

echo "== Cost of catching a write with a page fault"
{
    ./fault_cost -t 0
    ./fault_cost -t 19 | tail -1
    ./fault_cost -t 39 | tail -1
} > fault_cost.txt
cat fault_cost.txt

echo
echo "Done. Results: $OUT, fault_cost.txt   Summary: python3 summarize.py $OUT"

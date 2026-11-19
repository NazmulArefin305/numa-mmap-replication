#!/usr/bin/env bash
# Experiment 6: finding the hot pages automatically.
#
#   sudo ./run_exp6.sh          (about 30 minutes)
#
# Workload as in experiment 5: random 4 KiB reads of the 8 GiB file, the hot
# 10% of blocks receive 90% of reads, 40 threads over both sockets.
# Settings:
#   plain     plain shared mmap
#   known     replicas of the hot 10%, known in advance (experiment 5's "hot10")
#   adaptive  replicas chosen automatically by counting reads (area 10% per socket)
#
# Part A (steady hot set): each setting under memory limits of LIMITS GiB,
#   REPS runs of 30 s after a 10 s warm-up; plus one adaptive run from a cold
#   start with a per-second timeline (timeline_learn.csv).
# Part B (moving hot set): 60 s runs under a 12 GiB limit; the hot set moves to
#   the middle of the file after 20 s; per-second timelines
#   (timeline_move_<setting>_<run>.csv).
#
# Uses ../numa-mmap-replica/data8g.bin and the hot-10% replica files made by
# experiment 5 in ../numa-mmap-replica-v3/ (FILE and REP override them).
set -euo pipefail
cd "$(dirname "$0")"

FILE=${FILE:-../numa-mmap-replica/data8g.bin}
REP=${REP:-../numa-mmap-replica-v3/rep_hot10_}
LIMITS=${LIMITS:-"12 10"}
REPS=${REPS:-2}
THREADS=${THREADS:-40}
OUT=${OUT:-results_exp6.csv}
NB=/proc/sys/kernel/numa_balancing

[ -x ./numa_mmap_bench ] || make
[ -f "$FILE" ] || { echo "Data file $FILE not found. Set FILE=..." >&2; exit 1; }
[ -f "${REP}0.bin" ] && [ -f "${REP}1.bin" ] || {
    echo "Replica files ${REP}0.bin / ${REP}1.bin not found (made by run_exp5.sh). Set REP=..." >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "Run this with sudo." >&2; exit 1; }

nb_orig=$(cat "$NB")
trap 'echo "$nb_orig" > "$NB"' EXIT
echo 0 > "$NB"

# run_one LIMIT_GIB SETTING extra-options...
run_one() {
    local limit=$1 setting=$2 args=(); shift 2
    case "$setting" in
        plain)    args=(-m shared) ;;
        known)    args=(-m filerep -R "$REP") ;;
        adaptive) args=(-m adaptive -c 10) ;;
    esac
    sync; echo 3 > /proc/sys/vm/drop_caches
    systemd-run --scope --quiet -p MemoryMax="${limit}G" -p MemorySwapMax=0 \
        ./numa_mmap_bench -f "$FILE" "${args[@]}" -A -k 10:90 -t "$THREADS" "$@" >> "$OUT"
}

./numa_mmap_bench -H > "$OUT"

echo "== Correctness check: adaptive replicas, 10 s (every read compared with the file)"
run_one 12 adaptive -d 10 -C
errors=$(tail -1 "$OUT" | awk -F, '{print $21}')
if [ "$errors" != "0" ]; then
    echo "STOP: $errors adaptive reads returned wrong data. Send this output to Claude." >&2
    exit 1
fi
echo "   0 wrong reads."

echo "== Part A: steady hot set"
for limit in $LIMITS; do
    for setting in plain known adaptive; do
        for r in $(seq "$REPS"); do
            echo "-- limit ${limit} GiB, $setting, run $r of $REPS"
            run_one "$limit" "$setting" -d 30 -D 10
        done
    done
done
echo "-- learning curve: adaptive from a cold start, 20 s, limit 12 GiB"
run_one 12 adaptive -d 20 -o timeline_learn.csv

echo "== Part B: hot set moves after 20 s (60 s runs, limit 12 GiB)"
for setting in plain known adaptive; do
    for r in $(seq "$REPS"); do
        echo "-- $setting, run $r of $REPS"
        run_one 12 "$setting" -d 60 -s 20 -o "timeline_move_${setting}_${r}.csv"
    done
done

echo
echo "Done. Results: $OUT and timeline_*.csv"
echo "Summary: python3 summarize.py $OUT    Charts: python3 plot_timeline.py"

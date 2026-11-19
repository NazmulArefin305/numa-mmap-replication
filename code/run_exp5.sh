#!/usr/bin/env bash
# Experiment 5: replication when memory is limited (PaCaR limitation 2).
#
#   sudo ./run_exp5.sh check     quick test, about 2 minutes
#   sudo ./run_exp5.sh           full experiment, about 45 minutes
#
# Workload: random 4 KiB reads of an 8 GiB file; the first 10% of its blocks
# receive 90% of the reads. 40 threads over both sockets.
# Settings compared, each under a memory limit of LIMITS GiB:
#   plain    plain shared mmap                         (needs 8 GiB)
#   full     full replica file per socket              (needs 16 GiB)
#   hot10    replica of the hot 10% per socket + file  (needs about 9.6 GiB)
#   hot25    replica of the hot 25% per socket + file  (needs about 12 GiB)
# The replicas are ordinary files, so the kernel can evict them like PaCaR's
# twins. Every run starts with an empty page cache, so everything it reads
# counts against its limit.
#
# Settings: FILE (default ../numa-mmap-replica/data8g.bin), LIMITS
# ("24 16 12 10"), REPS (2), SECS (30), WARM (10), THREADS (40).
set -euo pipefail
cd "$(dirname "$0")"

FILE=${FILE:-../numa-mmap-replica/data8g.bin}
LIMITS=${LIMITS:-"24 16 12 10"}
REPS=${REPS:-2}
SECS=${SECS:-30}
WARM=${WARM:-10}
THREADS=${THREADS:-40}
OUT=${OUT:-results_exp5.csv}
NB=/proc/sys/kernel/numa_balancing

[ -x ./numa_mmap_bench ] || make
[ -f "$FILE" ] || { echo "Data file $FILE not found. Set FILE=..." >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "Run this with sudo." >&2; exit 1; }

nb_orig=$(cat "$NB")
trap 'echo "$nb_orig" > "$NB"' EXIT
echo 0 > "$NB"

# --- 1. replica files (created once, reused) ---
size=$(stat -c %s "$FILE")
need_mb=$(( (size * 2 + size / 10 * 2 + size / 4 * 2) / 1048576 ))
make_copy() {   # make_copy NAME BYTES
    local name=$1 bytes=$2
    if [ ! -f "$name" ] || [ "$(stat -c %s "$name")" -ne "$bytes" ]; then
        echo "   creating $name ($((bytes / 1048576)) MiB)"
        head -c "$bytes" "$FILE" > "$name"
    fi
}
if [ ! -f rep_full_0.bin ]; then
    free_mb=$(df -m --output=avail . | tail -1)
    if [ "$free_mb" -lt $((need_mb + 2048)) ]; then
        echo "Not enough disk space: need about $need_mb MiB, have $free_mb MiB." >&2
        exit 1
    fi
fi
echo "== Preparing replica files (only the first time takes a few minutes)"
blk=4096
hot10=$(( size / 10 / blk * blk ))
hot25=$(( size / 4 / blk * blk ))
for k in 0 1; do
    make_copy "rep_full_$k.bin" "$size"
    make_copy "rep_hot10_$k.bin" "$hot10"
    make_copy "rep_hot25_$k.bin" "$hot25"
done
sync

# --- 2. one measured run under a memory limit ---
# run_one LIMIT_GIB SETTING
run_one() {
    local limit=$1 setting=$2 args=()
    case "$setting" in
        plain) args=(-m shared) ;;
        full)  args=(-m filerep -R ./rep_full_) ;;
        hot10) args=(-m filerep -R ./rep_hot10_) ;;
        hot25) args=(-m filerep -R ./rep_hot25_) ;;
    esac
    sync; echo 3 > /proc/sys/vm/drop_caches
    systemd-run --scope --quiet -p MemoryMax="${limit}G" -p MemorySwapMax=0 \
        ./numa_mmap_bench -f "$FILE" "${args[@]}" -A -k 10:90 \
        -t "$THREADS" -d "$SECS" -D "$WARM" >> "$OUT"
}

if [ "${1:-}" = "check" ]; then
    OUT=results_exp5_check.csv
    ./numa_mmap_bench -H > "$OUT"
    echo "== Check 1: plain mmap with a 24 GiB limit (should fit, no disk reads)"
    SECS=10 WARM=3 run_one 24 plain
    echo "== Check 2: full replicas with a 10 GiB limit (should NOT fit: disk reads)"
    SECS=10 WARM=3 run_one 10 full
    limit=$(tail -1 "$OUT" | awk -F, '{print $NF}')
    echo
    if awk -v l="$limit" 'BEGIN { exit !(l > 9 && l < 11) }'; then
        echo "OK: the memory limit is applied (the run saw ${limit} GiB)."
    else
        echo "PROBLEM: the run did not see a 10 GiB limit (it saw '$limit'). Send this output to Claude." >&2
        exit 1
    fi
    exit 0
fi

# --- 3. the full experiment ---
./numa_mmap_bench -H > "$OUT"
for limit in $LIMITS; do
    for setting in plain full hot10 hot25; do
        for r in $(seq "$REPS"); do
            echo "== limit ${limit} GiB, $setting, run $r of $REPS"
            run_one "$limit" "$setting"
        done
    done
done

echo
echo "Done. Results: $OUT   Summary: python3 summarize.py $OUT"

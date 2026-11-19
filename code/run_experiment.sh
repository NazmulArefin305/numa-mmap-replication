#!/usr/bin/env bash
# Runs the whole experiment and writes one CSV row per run to results.csv.
#
#   sudo ./run_experiment.sh                # real run on a NUMA machine
#   SIZE_MB=256 SECS=3 REPS=1 FAKE=2 ./run_experiment.sh   # quick test in a VM
#
# Settings (environment variables):
#   FILE     data file to read            (default ./data.bin, created if missing)
#   SIZE_MB  size of the data file in MiB (default 2048)
#   THREADS  thread counts to test        (default: 2 4 8 ... up to all CPUs)
#   SECS     seconds per measurement      (default 20)
#   REPS     repetitions per setting      (default 3)
#   OUT      result file                  (default results.csv)
#   FAKE     pretend to have N NUMA nodes (testing only; default 0 = off)
#
# Run it with sudo on the real machine: root is needed to switch Linux's
# automatic NUMA balancing on and off and to clear the page cache.
set -euo pipefail
cd "$(dirname "$0")"

FILE=${FILE:-./data.bin}
SIZE_MB=${SIZE_MB:-2048}
SECS=${SECS:-20}
REPS=${REPS:-3}
OUT=${OUT:-results.csv}
FAKE=${FAKE:-0}
NB=/proc/sys/kernel/numa_balancing

[ -x ./numa_mmap_bench ] || make

if [ -z "${THREADS:-}" ]; then
    ncpu=$(nproc)
    THREADS=""
    for n in 2 4 8 16 32 64 128 256; do
        [ "$n" -lt "$ncpu" ] && THREADS="$THREADS $n"
    done
    THREADS="$THREADS $ncpu"
fi

if [ ! -f "$FILE" ] || [ "$(stat -c %s "$FILE")" -ne $((SIZE_MB * 1024 * 1024)) ]; then
    echo "Creating $FILE ($SIZE_MB MiB of random data)..."
    head -c "${SIZE_MB}M" /dev/urandom > "$FILE"
    sync
fi

extra=()
[ "$FAKE" -gt 0 ] && extra=(-F "$FAKE")

# Remember the NUMA balancing setting and put it back when we finish.
nb_orig=""
[ -r "$NB" ] && nb_orig=$(cat "$NB")
restore() { [ -n "$nb_orig" ] && [ -w "$NB" ] && echo "$nb_orig" > "$NB" || true; }
trap restore EXIT

set_balancing() {   # returns 1 if the setting cannot be changed
    [ -w "$NB" ] || return 1
    echo "$1" > "$NB"
}

drop_caches() {
    if [ -w /proc/sys/vm/drop_caches ]; then sync; echo 3 > /proc/sys/vm/drop_caches; fi
}

./numa_mmap_bench -H > "$OUT"

# run_config MODE WARM BALANCING
run_config() {
    local mode=$1 warm=$2 nb=$3 first=1
    if ! set_balancing "$nb"; then
        if [ "$nb" = 1 ]; then
            echo "SKIPPED $mode/$warm with NUMA balancing on: cannot change $NB (run with sudo)." >&2
            return
        fi
        echo "note: cannot change $NB; it stays at '${nb_orig:-unavailable}'." >&2
    fi
    drop_caches
    for t in $THREADS; do
        for _ in $(seq "$REPS"); do
            # Evict the file on the first run of each setting so that this
            # setting decides where the page cache pages are placed.
            local e=()
            [ "$first" = 1 ] && e=(-e)
            first=0
            ./numa_mmap_bench -f "$FILE" -m "$mode" -w "$warm" -t "$t" -d "$SECS" \
                "${e[@]}" "${extra[@]}" >> "$OUT"
        done
    done
}

echo "== Baseline 1: plain mmap, file pages spread over all nodes"
run_config shared spread 0
echo "== Baseline 2: the same, with Linux automatic NUMA balancing on"
run_config shared spread 1
echo "== Baseline 3: plain mmap, all file pages on one node"
run_config shared single 0
echo "== Improvement: one replica per NUMA node"
run_config replica spread 0

echo
echo "Done. Results are in $OUT. Plot them with: python3 plot_results.py $OUT"

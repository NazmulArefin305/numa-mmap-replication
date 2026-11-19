# Per-node replicas for memory-mapped files on NUMA

A course-project starter that extends the idea of PaCaR (EuroSys '26) to the case
PaCaR leaves out: files accessed through `mmap`.

PaCaR keeps one copy of a page-cache page per NUMA node, but only for `read()` and
`write()`. Mapped files get no replication. This project measures that gap and closes
it in user space: the benchmark keeps one private copy of the file on each NUMA node
and lets every thread read the copy on its own node.

## Files

| File | Purpose |
|---|---|
| `numa_mmap_bench.c` | The benchmark. `-m shared` is the baseline (plain `mmap`), `-m replica` is the improvement. |
| `Makefile` | Builds it with `make`. No libraries needed beyond a C compiler. |
| `run_experiment.sh` | Runs every setting, several thread counts and repetitions, into `results.csv`. |
| `plot_results.py` | Draws `results.png` and prints a speed-up table. |

## What the experiment compares

| Setting | What it represents |
|---|---|
| mmap, pages spread over nodes | Stock Linux when threads on all nodes loaded the file. About half of all accesses are remote on a two-node machine. |
| mmap, spread + NUMA balancing | The same with Linux's automatic NUMA balancing switched on, which the paper says already covers mapped files. |
| mmap, all pages on one node | Stock Linux when one thread loaded the file. |
| Per-node replicas | This project. Every access is local, at the cost of one copy per node. |

Each run reports read throughput (GiB/s) and the share of accesses served from the
thread's own node. The second number is measured by asking the kernel where a sample
of pages is, so it shows *why* the throughput differs.

## Step 1: check that it works on your Mac (no results yet)

macOS cannot run this directly, because it uses Linux system calls. Use a Linux VM.

1. Install UTM (free) and create an Ubuntu Server 24.04 VM. On an Apple Silicon Mac
   choose the ARM64 image; the code is not tied to x86.
2. In the VM: `sudo apt update && sudo apt install -y build-essential python3-matplotlib unzip`
3. Copy the project in, then run a two-minute test:

   ```
   make
   SIZE_MB=256 SECS=3 REPS=1 FAKE=2 ./run_experiment.sh
   python3 plot_results.py results.csv
   ```

`FAKE=2` makes the program pretend there are two NUMA nodes so that every code path
runs. The numbers from this test mean nothing: a VM on a laptop has no remote memory,
so both modes perform alike. Use it only to confirm that everything builds and runs.

## Step 2: get a real two-socket machine online

You need a machine where `lscpu | grep NUMA` reports two (or more) nodes. Because
this project runs in user space, any stock Ubuntu works; you do not need to install
the PaCaR kernel.

### Option A: CloudLab (free, needs your instructor)

CloudLab gives real bare-metal servers to academic users at no cost. Students cannot
open a project themselves: the instructor (or a TA) applies for a class project, and
students then join it. Approval can take a few days, so ask early.

1. Ask your instructor to apply at https://www.cloudlab.us (Start New Project, marked
   as a class).
2. Once it is approved, sign up with "Join Existing Project" and the project name.
   Upload your SSH public key during sign-up.
3. Start an experiment with one node and an Ubuntu image, choosing a hardware type
   with two CPU sockets (the hardware page lists them).
4. Log in with the SSH command shown in the experiment's List View, then go to Step 3.

Storage on CloudLab nodes is erased when the experiment ends. Copy `results.csv` and
`results.png` to your Mac before it expires.

### Option B: a rented cloud instance (paid, no sponsor needed)

On AWS, the `c5.24xlarge` instance (96 virtual CPUs, 192 GiB) exposes two NUMA nodes;
`c5.metal` is the same hardware without a hypervisor. The on-demand price was about
4 USD per hour in the cheapest regions when this was written (November 2026). Check
the current price before you launch. One full run of the experiment takes well under
an hour.

1. Create an AWS account. New accounts usually have a low limit on virtual CPUs.
   Open Service Quotas, find the EC2 quota for running on-demand standard instances,
   and request at least 96 vCPUs. This can take from hours to a couple of days.
2. In EC2, launch an instance: Ubuntu Server 24.04 (x86), type `c5.24xlarge`, a new
   key pair (download the `.pem` file), 30 GB of storage, SSH allowed from your IP.
3. From the Mac terminal:

   ```
   chmod 400 mykey.pem
   scp -i mykey.pem numa-mmap-replica.zip ubuntu@<public-ip>:~
   ssh -i mykey.pem ubuntu@<public-ip>
   ```
4. Go to Step 3. When you have copied the results back, **terminate the instance**,
   otherwise it keeps charging.

## Step 3: run the experiment on that machine

```
sudo apt update && sudo apt install -y build-essential numactl python3-matplotlib unzip tmux
lscpu | grep NUMA            # must show 2 or more NUMA nodes
numactl --hardware           # shows the CPUs and memory of each node

unzip numa-mmap-replica.zip && cd numa-mmap-replica
make
tmux                         # keeps the run alive if your connection drops
sudo ./run_experiment.sh     # about 30-40 minutes with the default settings
python3 plot_results.py results.csv
```

`sudo` is needed because the script switches NUMA balancing on and off and clears the
page cache. It restores the original NUMA balancing setting when it ends.

Then, from the Mac, fetch the results:

```
scp -i mykey.pem "ubuntu@<public-ip>:~/numa-mmap-replica/results.*" .
```

Useful settings, given as environment variables in front of the command:

| Variable | Default | Meaning |
|---|---|---|
| `SIZE_MB` | 2048 | Data file size. Replica mode needs this much free memory on every node. |
| `THREADS` | 2, 4, 8, ... up to all CPUs | Thread counts to test, e.g. `THREADS="8 24 48 96"`. |
| `SECS` | 20 | Seconds per measurement. |
| `REPS` | 3 | Repetitions per setting. |

Example: `sudo SIZE_MB=8192 REPS=5 ./run_experiment.sh`

## What to expect, and what to check

The code has been compiled and run end to end, but only on a machine with a single
NUMA node. It has not been measured on real two-socket hardware, so the size of the
gain is for you to find out. The paper reports 60-190% higher latency and up to 80%
lower bandwidth for remote memory, and up to 1.4-1.6x for its own replication on
read-only workloads, so a clear gain at high thread counts is the reasonable
expectation.

If the replicas do not win, check these in order. First, the locality panel: the
mmap baselines should be near 50% local and the replicas near 100%. If the baselines
are already near 100%, the file was not spread as intended. Second, the thread count:
with few threads the interconnect is not saturated and the gap is smaller. Third, the
data size: it must be much larger than the CPU caches (the 2 GiB default is).

## Limitations to state in your report

| Limitation | Why it matters |
|---|---|
| Not transparent | A program must use this code path; PaCaR needs no application change. |
| Read-only | There is no write handling. PaCaR's main/twin invalidation is the model to follow. |
| Eager, whole-file copies | Memory use is file size times the number of nodes, and set-up time grows with it. PaCaR replicates lazily, per page, and gives memory back under pressure. |
| Synthetic workload | Uniform random 4 KiB reads; no real application yet. |
| Pinned threads | Threads are fixed to their node, so NUMA balancing cannot help by moving threads. |

## Ideas for going further

A natural next step is write support: keep the replicas read-only with `mprotect`,
catch the fault on a write, apply the write to one main copy and mark the other
copies stale, as PaCaR does. A second is lazy replication with `userfaultfd`, copying
a page to a node only when a thread on that node first reads it.

## Version 2: experiments 1, 3 and 4

Version 2 keeps everything above and adds new modes and three run scripts. It
unpacks into its own folder, `numa-mmap-replica-v2`, next to the first one, and
by default reads the 2 GiB `data.bin` from `../numa-mmap-replica/`.

| Script | Experiment | Time | Output |
|---|---|---|---|
| `sudo ./run_exp1.sh` | 1a, 1b, 1c: does NUMA balancing look at the mapped file? | ~5 min | `vmstat_1a.txt`, `vmstat_1b.txt`, `vmstat_1c.txt`, `results_exp1.csv` |
| `sudo ./run_exp3.sh` | 3: unpinned threads | ~20 min | `results_exp3.csv` |
| `sudo ./run_exp4.sh` | 4: reads and writes with the main/twin rule, plus fault cost | ~35 min | `results_exp4.csv`, `results_exp4_check.csv`, `fault_cost.txt` |

`python3 summarize.py <file.csv>` prints an averaged table of any results file,
and for experiment 4 the replica speed-up per read share.

New benchmark options: `-m anon` (NUMA balancing control), `-u` (unpinned
threads), `-n NODE` (all threads on one node), `-W` (map the file read-write),
`-r PCT` (share of reads), `-P` (main/twin protocol even at 100% reads), `-C`
(check that no read is torn), `-S N` (sample locality every N operations).
`fault_cost` times a write that is caught by a page fault, the mechanism
PaCaR's proposed transparent mmap design would need.

Experiment 4 writes into the data file, so its contents change.

## Version 3: experiment 5 (limited memory, PaCaR limitation 2)

| Command | What it does | Time |
|---|---|---|
| `sudo ./run_exp5.sh check` | Makes the replica files (first time only), then two 10 s runs that confirm the memory limit works | ~5 min the first time |
| `sudo ./run_exp5.sh` | Plain mmap, full replicas, and replicas of the hot 10% and 25%, each under memory limits of 24, 16, 12 and 10 GiB | ~45 min |

It uses the 8 GiB `../numa-mmap-replica/data8g.bin` and about 22 GiB of extra
disk space for the replica files. `python3 summarize.py results_exp5.csv`
prints throughput, speed-up and disk reads per memory limit.

New options: `-m filerep -R PREFIX` (replicas as evictable files
PREFIX0.bin, PREFIX1.bin; a shorter file replicates only the first, hot part),
`-k H:A` (the first H% of blocks get A% of reads), `-A` (no read-ahead on
file mappings), `-D SECS` (warm-up before measuring). New CSV columns:
`hot_pct`, `hot_access_pct`, `rep_frac`, `disk_read_mb_s`, `majflt_per_s`,
`mem_limit_gib`.

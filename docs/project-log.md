# OS course project log: mmap replication on NUMA (based on PaCaR, EuroSys '26)

Last updated: 2026-11-19. Read this first. The code guide is [`../code/README.md`](../code/README.md). The detailed plan and all results are in [`experiment-plan.md`](experiment-plan.md).

## 1. Project idea

PaCaR replicates page-cache pages on each NUMA node, but only for `read()`/`write()`. Its Section 7 lists two limitations:

| # | Limitation | Paper's proposed direction |
|---|---|---|
| 1 | No support for memory-mapped (mmap) pages: accesses bypass the kernel, so PaCaR skips any mapped page (mapcount > 0). Paper adds that NUMA balancing "already targets mmaped pages". | Per-node page tables pointing to local replicas; write-protect replicas so writes fault and the kernel keeps copies consistent. Called out of scope. |
| 2 | Replication shrinks effective page-cache size; large datasets that can't be fully replicated may hit disk more. | Replicate only the most frequently accessed pages. |

Our project targets limitation 1. Course requirement: show an improvement. Approach chosen: a user-space prototype (no kernel work), measured against stock Linux mmap.

## 2. What was built

`numa-mmap-replica.zip`: `numa_mmap_bench.c` (modes `shared` = plain mmap baseline, `replica` = one private copy of the file per NUMA node), `run_experiment.sh` (runs all settings), `plot_results.py` (chart + speed-up table), `README.md`. No libraries beyond a C compiler.

## 3. Infrastructure decisions

| Option | Outcome |
|---|---|
| Mac / Windows VM | Only for development; a single-socket PC has no remote memory, so throughput results would be meaningless (emulated NUMA can show locality only). |
| KFUPM AWS Innovation Sandbox | Possible: Oregon (us-west-2) only; Testing $10/7 days, Standard $35/14 days, Extended $55/1 month; 96-vCPU quota must be requested via CCM-Admin. c5.24xlarge ≈ $4/h, exposes 2 NUMA nodes. Not used. |
| CloudLab | Used. Free, real bare metal. |

## 4. CloudLab setup (done)

Project: `numammapresearch` (Muhammed is leader; description "NUMA-Aware Memory-Mapped File Benchmark"). Projects have no documented expiry; experiments do.

Teammates join by: cloudlab.us → Sign Up → Join Existing Project → `numammapresearch` → upload their own SSH public key. Leader approves via email or the project's Members tab. Approve them before starting an experiment.

Starting an experiment: Experiments → Start Experiment → profile `small-lan` → 1 node, Ubuntu 24.04, hardware type `c220g5` → name `numa-bench`, project `numammapresearch` (cluster auto-selected: Wisconsin) → start now, duration 16 h → Finish. Only 1 CloudLab node is needed: one server holds both NUMA nodes.

Dual-socket alternatives if c220g5 is busy: c220g2 (Wisconsin), c6420 or c6320 (Clemson).

Experiments expire (default here 16 h; Extend button for more). Disk is erased at expiry. Each new experiment is a fresh machine: copy the zip, install tools, build again.

## 5. SSH access (what worked)

The original key `~/.ssh/id_ed25519` had a forgotten passphrase. A new key was made: `ssh-keygen -t ed25519 -f ~/.ssh/cloudlab_key`, its `.pub` added in CloudLab (name menu → Manage SSH Keys) and, because the machine was already running, also appended to `~/.ssh/authorized_keys` through the browser shell (node0 → Shell). Username is `Arefin` (capital A). The browser shell times out ("Connection terminated"); use the Mac Terminal instead.

Commands (on the Mac; prompt starts `muhammednazmularefin@...`):

```
scp -i ~/.ssh/cloudlab_key numa-mmap-replica.zip Arefin@<host>:~
ssh -i ~/.ssh/cloudlab_key Arefin@<host>
scp -i ~/.ssh/cloudlab_key "Arefin@<host>:~/numa-mmap-replica/results.*" ~/Downloads/
```

Common mistakes made: running `scp` while logged in to the server (prompt `Arefin@node0`); typing `wic` instead of `wisc` in the host name; the zip not yet downloaded to the Mac.

On the server:

```
sudo apt update && sudo apt install -y build-essential numactl python3-matplotlib unzip tmux
lscpu | grep NUMA          # must show 2
unzip numa-mmap-replica.zip && cd numa-mmap-replica && make
tmux                        # Ctrl+B then D to detach; `tmux attach` to return
sudo ./run_experiment.sh
python3 plot_results.py results.csv
```

Note: later on 19 Nov, `apt install` started failing on this machine ("Network is unreachable" to us.archive.ubuntu.com), so `stress-ng` could not be installed. Tools already installed keep working.


## 6. Run 1 (2026-11-19, host c220g5-110414.wisc.cloudlab.us)

Machine: 2 × Xeon Silver 4114, 2 NUMA nodes (node0 CPUs 0-9,20-29; node1 10-19,30-39), Ubuntu 24.04.5, kernel 6.8. 2 GiB file, random 4 KiB reads, threads 2-40, 3 reps × 20 s, 4 settings = 72 measurements. Full table in `results-run1-c220g5.md`.

Headline: replicas 1.35-1.49x faster than best mmap at every thread count (86.7 vs 58.3 GiB/s at 40 threads); local accesses 100% vs 50%; NUMA balancing changed throughput by <1%; baselines plateau from 16 threads.

Files: `results.csv`, `results.png` (on the Mac; `results.png` also embedded in the Experiment Plan doc), `experiment_diagram.png` (in `figures/`: panel A baseline split file + busy cross-socket link; panel B one full replica per node, link unused; strip listing all 4 settings).

### Machine configuration (from `sysinfo.txt`, 19 Nov)

2 × Xeon Silver 4114 @ 2.20 GHz (Skylake-SP), 10 cores × 2 threads per socket = 40 CPUs, 800-3000 MHz; L1d/L1i 32 KiB/core, L2 1 MiB/core, L3 13.75 MiB/socket. NUMA node 0: CPUs 0-9,20-29, 95,303 MB; node 1: CPUs 10-19,30-39, 96,757 MB; distances 10/21. Memory: 6 × 32 GB DDR4 (rated 2666, configured 2400 MT/s), ECC, 18 of 24 slots empty (likely 3 of 6 channels per socket, ≈57.6 GB/s peak per socket; inference). Swap 7 GB. Disks: sda Intel SSD SSDSC2BB48 447 GB (root /dev/sda3 63 GB, 59% used), sdb Seagate ST1200MM0088 1.1 TB HDD unused. Ubuntu 24.04.5, kernel 6.8.0-149-generic, gcc 13.3.0, Python 3.12.3, 4 KiB pages, THP madvise, PTI/IBRS mitigations on. Full write-up in the Experiment Plan doc, section "Resource configuration".

## 7. Experiment 1a (2026-11-19, ~15:30 Riyadh): NUMA balancing counters

Setup: NUMA balancing on (`echo 1 > /proc/sys/kernel/numa_balancing`), `./numa_mmap_bench -f data.bin -m shared -t 40 -d 60` (read-only shared mapping of the 2 GiB file). Change in machine-wide `/proc/vmstat` counters over the run (saved as `vmstat_1a.txt` on the server):

| Counter | Change in 60 s |
|---|---|
| numa_pte_updates | 452 |
| numa_hint_faults | 454 |
| numa_hint_faults_local | 319 |
| numa_pages_migrated | 95 |

Benchmark result in the same run: 57.88 GiB/s, 50% local, set-up 7.6 s (same as run 1 without balancing).

Interpretation: the file has 524,288 pages; balancing marked only 452 pages anywhere on the machine (0.09% of the file's page count), although one scan normally covers up to 256 MB (65,536 pages). This supports H1a: balancing essentially ignores read-only file-backed mappings. Counters are machine-wide, so the few hundred pages may be other memory (thread buffers etc.). Experiment 1b (same file mapped writable) is needed to confirm. Experiment 1c (stress-ng positive control) was skipped because apt could not download packages; a built-in positive-control mode will be added to the new code instead.

### Experiment 1a, 1b, 1c with `run_exp1.sh` (16:30, folder ~/numa-mmap-replica-v2)

| Run | pte_updates | hint_faults (local) | pages_migrated | Local | GiB/s |
|---|---|---|---|---|---|
| 1a read-only mapping, 40 thr | 463 | 464 (316) | 104 | 50.0% | 57.87 |
| 1b read-write mapping, no writes, 40 thr | 1,700,140 | 1,700,260 (850,306) | 554,226 | 49.9% | 57.51 |
| 1c anon on node 0, 20 thr on node 1, balancing off | – | – | – | 0.0% | 28.63 |
| 1c same, balancing on | 1,531,529 | 1,531,566 (1,007,192) | 524,320 | 94.6% | 44.25 |

Confirmed: balancing skips read-only file mappings (1a vs 1b: >3,000x more activity just from the writable flag). When it scans (1b) it migrates more pages than the file has (pages bounce between sockets) with no locality gain and slightly lower throughput. Positive control (1c) works: whole region migrated, 0% → 94.6% local, +55% throughput. So balancing works for single-socket access but cannot help data shared by both sockets; PaCaR's Section 7 reassurance fails for shared mapped files.

## 7b. Experiment 2 (2026-11-19, 15:35-16:20): 8 GiB file

Same as run 1 with `SIZE_MB=8192 FILE=./data8g.bin OUT=results_8g.csv` (kept the 2 GiB `data.bin`). Files `results_8g.csv`, `results_8g.png` (copied to the Mac; chart embedded in the Experiment Plan doc).

| Threads | Best mmap 8 GiB (GiB/s) | Replicas 8 GiB | Speed-up 8 GiB | Speed-up 2 GiB |
|---|---|---|---|---|
| 2 | 10.3 | 14.2 | 1.38x | 1.38x |
| 4 | 19.7 | 27.0 | 1.37x | 1.39x |
| 8 | 34.8 | 47.4 | 1.36x | 1.35x |
| 16 | 49.5 | 71.8 | 1.45x | 1.35x |
| 32 | 54.6 | 84.1 | 1.54x | 1.48x |
| 40 | 56.7 | 85.5 | 1.51x | 1.49x |

H2 holds. Plain mmap lost more from the bigger file (−11.5% at 16 threads vs run 1) than replicas (−4.8%), so the speed-up rose; possible cause more TLB misses (not measured). NUMA balancing <1.4% difference. One-node mmap 34.1 GiB/s at 40 threads (41.6 in run 1). Replicas used 16 GiB; set-up ~46 s per run vs 31 s first cold baseline run. Spread <2% except 8 threads (5.6%).

## 7c. Experiment 3 (16:38-17:15): unpinned threads (`run_exp3.sh`, `results_exp3.csv`)

| Threads | mmap unpinned NB off | mmap unpinned NB on | replicas unpinned | replicas pinned | speed-up unpinned |
|---|---|---|---|---|---|
| 8 | 37.59 | 37.63 | 52.55 | 53.44 | 1.40x |
| 20 | 56.69 | 56.73 | 79.95 | 80.26 | 1.41x |
| 40 | 58.24 | 58.29 | 86.66 | 86.69 | 1.49x |

GiB/s, mean of 3 × 20 s. Sampled locality: mmap ~50%, replicas 100%. Unpinned replicas within 2% of pinned (−1.7% at 8 thr, <0.5% at 20/40). Balancing <0.1% effect. Node changes only 0.01-0.03 per thread per second (about once a minute), so frequent-migration behaviour is untested; optional stress test.

## 7d. Experiment 4 (17:20-18:30): writes with main/twin protocol (`run_exp4.sh`; `results_exp4.csv`, `results_exp4_check.csv`, `fault_cost.txt`)

Correctness check: 50% writes, 40 threads, 10 s, 2.4M twin refreshes/s → 0 torn reads.

| Reads | Speed-up 16 thr | Speed-up 40 thr | Replica local | Refreshes 40 thr (M/s) |
|---|---|---|---|---|
| 100% | 1.30x | 1.44x | 100% | 0 |
| 99% | 1.33x | 1.46x | 98.5% | 0.37 |
| 95% | 1.16x | 1.24x | 93.0% | 1.37 |
| 90% | 1.00x | 1.05x | 86.9% | 2.04 |
| 75% | 0.77x | 0.84x | 72.4% | 2.72 |
| 65% | 0.73x | 0.77x | 65.5% | 2.69 |
| 50% | 0.73x | 0.78x | 58.3% | 2.45 |

Speed-up = replica ops/s ÷ plain mmap ops/s, same mix; 3 × 20 s; spread <2.5%. Crossover 87-90% reads (interpolated 89.8% at 16 thr, 86.5% at 40), vs PaCaR's 65%. Protocol overhead at 100% reads vs read-only replicas (run 1): 5.3% (16 thr), 2.9% (40 thr). Likely reason for early crossover (not measured): each write stales both twins, next read per socket pays remote read + 4 KiB copy; with mmap an op is only a memory copy, whereas read()/write() in PaCaR add a syscall that hides this cost. Chart `exp4_speedup.png` (in `figures/`).

Fault cost (`fault_cost`): plain write 33 / 98 / 74 ns; write to protected page 6.5 / 12.0 / 19.6 µs; re-protect one page 2.4 / 7.8 / 14.4 µs, with 0 / 19 / 39 other threads. 120-270x a plain write; grows with threads (TLB shootdowns). At 40 threads one protect+fault cycle ≈ 34 µs → ~1.2M such writes/s for all threads, vs ~3.7M writes/s the user-space replicas did at 50% reads. A transparent design needs most writes not to fault (e.g. leave hot written pages unreplicated).

Hypotheses: H4a holds, H4b partly (crossover exists but at 87-90%, not 50-75%), H4c holds.

## 7e. Experiment 5 (planned 19 Nov evening): limited memory, PaCaR limitation 2

Plan in the Experiment Plan doc ("Experiment 5: limited memory"). Code `numa-mmap-replica-v3.zip` (unpacks to `~/numa-mmap-replica-v3`): new mode `-m filerep -R PREFIX` (replicas as evictable files, a shorter replica file replicates only the hot first part), `-k 10:90` hot/cold reads, `-A` no read-ahead, `-D` warm-up; reports disk MB/s, major faults/s, cgroup memory limit. Script `run_exp5.sh` (`check` mode first): 8 GiB `data8g.bin`, 40 threads, settings plain / full / hot10 / hot25 under `systemd-run` MemoryMax 24, 16, 12, 10 GiB, page cache dropped before each run, 2 reps × 30 s after 10 s warm-up. Needs ~22 GiB disk for replica files. Hypotheses: H5a all fit at 24 GiB, full wins; H5b full falls behind plain at ≤16 GiB (disk reads); H5c selective fits to 10 GiB and keeps most of the gain. Hot set known in advance (upper bound for selective replication). User asked to Extend the CloudLab experiment past 21:33.

Check run (19:15): plain at 24 GiB 59.5 GiB/s, no disk; full at 10 GiB 4.07 GiB/s with disk reads; cgroup limit confirmed (10.00 GiB).

Result (19:20-20:10, `results_exp5.csv`; chart `exp5_memory.png`):

| Limit | Plain | Full | Hot 10% | Hot 25% |
|---|---|---|---|---|
| 24 GiB | 59.52 | 88.37 (1.48x) | 83.52 (1.40x) | 84.41 (1.42x) |
| 16 GiB | 59.52 | 87.00 (1.46x), disk 61 MB/s | 83.50 (1.40x) | 84.41 (1.42x) |
| 12 GiB | 59.52 | 7.17 (0.12x), disk 275 MB/s | 83.51 (1.40x) | 84.42 (1.42x) |
| 10 GiB | 59.52 | 3.60 (0.06x), disk 276 MB/s | 83.54 (1.40x) | 83.49 (1.40x), disk 38 MB/s |

GiB/s, mean of 2 × 30 s after 10 s warm-up. H5a holds; H5b holds from 12 GiB (not at 16 GiB, where full replicas just fit); H5c holds. SSD plateau ~275 MB/s (~67k 4 KiB reads/s) caps full replicas. Hot-10% replicas: 1.6 GiB extra instead of 8 GiB, 83% of full replication's gain. Upper bound: hot set known in advance.

## 8. Interpretation and conclusion (from run 1)

The results confirm PaCaR's core idea (replication helps shared read-heavy data, same 1.4-1.6x range the paper reports) and show its mmap limitation has a real cost: about a third of achievable throughput is lost.

They do not support the paper's Section 7 reassurance that NUMA balancing covers mmap: balancing moves the single copy of a page, so when both sockets read the same pages, half the readers stay remote wherever it sits. The paper's own Section 3 (RocksDB with mmap + NUMA balancing still >50% remote) agrees with us. Experiment 1a adds evidence that balancing does not even scan the read-only mapping.

This experiment does not test PaCaR itself (patched kernel not run), so it says nothing about PaCaR's own robustness.

Our prototype vs PaCaR: user space vs kernel; not transparent vs transparent; mmap access vs read()/write() only; read-only vs handles writes (main/twin); eager whole-file copy vs lazy per-page; always 2× memory vs frees replicas under pressure. Limitation 2 not tested (2 GiB file fit easily even twice).

Caveats for the report: 2× memory; replica set-up ~12.5 s vs ~7.7 s cold baseline (per-run 11.5 s is partly a code artifact: the file is evicted after building replicas); read-only; synthetic uniform random workload; pinned threads.

## 9. Next steps

| Step | Status |
|---|---|
| Experiment 1a: balancing counters, read-only mapping | Done (section 7) |
| Experiment 1c: stress-ng positive control | Skipped (no package download); replace with built-in mode in new code |
| Experiment 2: 8 GiB file | Done (section 7b) |
| New code: `numa-mmap-replica-v2.zip` (unpacks to `~/numa-mmap-replica-v2`; scripts `run_exp1.sh` for 1a/1b/1c, `run_exp3.sh`, `run_exp4.sh`, `summarize.py`, `fault_cost`) | Written and tested; sent 16:00; being copied to the server |
| Experiments 1b, 3, 4 | After new code |
| Report and slides | Not started |
| Copy all results to the Mac, then terminate (machine expires 9:33 PM Riyadh, 19 Nov) | Pending |

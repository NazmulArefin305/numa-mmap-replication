/*
 * numa_mmap_bench.c  (version 2)
 *
 * Measures throughput on a memory-mapped file when threads on several NUMA
 * nodes access it. Modes:
 *
 *   shared   Baseline. One ordinary mmap() of the file, shared by all threads.
 *            This is stock Linux, and what PaCaR (EuroSys '26) leaves alone.
 *
 *   replica  One copy of the file per NUMA node; each thread uses the copy on
 *            its own node.
 *              - read-only (-r 100, the default): the copies are read-only,
 *                as in run 1.
 *              - with writes (-r below 100): PaCaR's main/twin rule. The file
 *                mapping is the main copy and takes every write; the per-node
 *                copies are twins. Each 4 KiB block has a version number; a
 *                write bumps the main version, which makes every twin of that
 *                block stale. A read uses the local twin only if its version
 *                matches the main one; otherwise it reads the main copy and
 *                refreshes the local twin (replicate-on-read).
 *
 *   filerep  Replicas as ordinary files that the kernel can evict (used for
 *            limitation 2): socket k reads PREFIXk.bin (-R PREFIX). If that
 *            file is shorter than the data file, only its first part (the hot
 *            blocks) is replicated and the rest is read from the shared file.
 *
 *   anon     Positive control for NUMA balancing. Anonymous memory, filled by
 *            a thread on the first node; use -n 1 to run every reader on the
 *            second node. With NUMA balancing on, Linux should move the pages
 *            over; this proves balancing works on this machine.
 *
 * Other options:
 *   -u       threads are not pinned; the scheduler places and moves them. In
 *            replica mode each thread checks its current CPU every 64
 *            operations and switches to that node's copy.
 *   -n IDX   pin every thread to NUMA node IDX (0, 1, ...).
 *   -W       map the file read-write even for a read-only workload.
 *   -r PCT   share of operations that are reads (default 100).
 *   -P       replica mode: use the main/twin protocol even at 100% reads,
 *            so its overhead can be measured on its own.
 *   -C       consistency check: writers stamp each block, readers verify that
 *            no read returns a half-old, half-new block (replica mode).
 *   -k H:A   hot/cold access: the first H% of blocks receive A% of accesses.
 *   -A       advise the kernel that file mappings are read randomly (no
 *            read-ahead), so disk reads reflect real misses.
 *   -D SECS  run the workload this long before measuring (warm-up).
 *   -S N     sample locality every N operations per thread (default 4096,
 *            0 = off): the thread records its node and the node of the page
 *            it just accessed. Works for pinned and unpinned threads.
 *
 * No extra libraries are needed: topology comes from /sys, and mbind /
 * move_pages are called as raw system calls.
 *
 * Build:  make
 * Run:    ./numa_mmap_bench -f data.bin -m replica -t 40 -d 20
 *
 * The last line printed on stdout is one CSV row (-H prints the header).
 * Warning: runs with writes modify the data file itself.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAX_NODES 64
#define CHUNK (2UL << 20)   /* warm-up granularity: 2 MiB */
#define SAMPLES 20000       /* pages sampled for the placement-based locality */
#define MAGIC 0xC0FFEE11UL  /* stamp used by the consistency check */
#define BUSY 1UL            /* twin version while it is being refreshed */
#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define cpu_relax() __asm__ __volatile__("yield")
#else
#define cpu_relax() do { } while (0)
#endif

struct node {
    int id;          /* kernel node id */
    cpu_set_t cpus;  /* CPUs of this node that we are allowed to use */
};

static struct node nodes[MAX_NODES];
static int nnodes;
static int cpu_node[CPU_SETSIZE];  /* CPU number -> index into nodes[] */
static int fake_nodes;
static long page_size;

static const char *file_path;
static int fd = -1;
static size_t file_size;
static size_t block = 4096;
static size_t nblocks;

enum { M_SHARED, M_REPLICA, M_ANON, M_FILEREP };
static int mode = M_SHARED;
static int read_pct = 100;
static int writable;          /* file mapped read-write */
static int rw_replica;        /* replica mode with writes: main/twin protocol */
static int unpinned;
static int all_on = -1;       /* -n: every thread on this node index */
static int check;
static int force_proto;      /* -P: main/twin protocol even with no writes */
static long sample_every = 4096;
static const char *rep_prefix;    /* -R: replica files PREFIX0.bin, PREFIX1.bin, ... */
static size_t nrep_blocks;        /* blocks covered by the file replicas */
static int hot_pct, hot_access;   /* -k H:A */
static size_t hot_blocks;
static int rand_advice;           /* -A */
static double warm_secs;          /* -D */

static unsigned char *region[MAX_NODES]; /* what node k's threads read */
static unsigned char *main_map;           /* the file mapping (main copy) */
static _Atomic unsigned long *ver_main;   /* per-block main version */
static _Atomic unsigned long *ver_rep[MAX_NODES]; /* per-block twin versions */

static atomic_int stop_flag;
static pthread_barrier_t start_barrier;

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------- NUMA topology ---------- */

static void parse_cpulist(const char *s, cpu_set_t *set)
{
    CPU_ZERO(set);
    while (*s && *s != '\n') {
        char *end;
        long a = strtol(s, &end, 10);
        long b = a;
        if (end == s)
            break;
        if (*end == '-')
            b = strtol(end + 1, &end, 10);
        for (long c = a; c <= b && c < CPU_SETSIZE; c++)
            CPU_SET(c, set);
        s = (*end == ',') ? end + 1 : end;
    }
}

static void discover_nodes(void)
{
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        die("sched_getaffinity");

    if (fake_nodes > 0) {
        int k = 0;
        nnodes = fake_nodes;
        for (int i = 0; i < nnodes; i++) {
            nodes[i].id = i;
            CPU_ZERO(&nodes[i].cpus);
        }
        for (int c = 0; c < CPU_SETSIZE; c++)
            if (CPU_ISSET(c, &allowed))
                CPU_SET(c, &nodes[k++ % nnodes].cpus);
        for (int i = 0; i < nnodes; i++)
            if (CPU_COUNT(&nodes[i].cpus) == 0)
                nodes[i].cpus = allowed;
    } else {
        for (int id = 0; id < MAX_NODES; id++) {
            char path[128], line[4096];
            snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", id);
            FILE *f = fopen(path, "r");
            if (!f)
                continue;
            if (fgets(line, sizeof(line), f)) {
                cpu_set_t set;
                parse_cpulist(line, &set);
                CPU_AND(&set, &set, &allowed);
                if (CPU_COUNT(&set) > 0) {
                    nodes[nnodes].id = id;
                    nodes[nnodes].cpus = set;
                    nnodes++;
                }
            }
            fclose(f);
        }
        if (nnodes == 0) {
            nodes[0].id = 0;
            nodes[0].cpus = allowed;
            nnodes = 1;
        }
    }
    for (int c = 0; c < CPU_SETSIZE; c++) {
        cpu_node[c] = 0;
        for (int i = 0; i < nnodes; i++)
            if (CPU_ISSET(c, &nodes[i].cpus)) {
                cpu_node[c] = i;
                break;
            }
    }
}

static int current_node(void)
{
    int c = sched_getcpu();
    return (c >= 0 && c < CPU_SETSIZE) ? cpu_node[c] : 0;
}

static void pin_to_node(int k)
{
    if (sched_setaffinity(0, sizeof(cpu_set_t), &nodes[k].cpus) != 0)
        perror("warning: sched_setaffinity");
}

static void bind_to_node(void *addr, size_t len, int k)
{
    unsigned long mask[2] = {0, 0};
    if (fake_nodes > 0)
        return;
    mask[nodes[k].id / 64] |= 1UL << (nodes[k].id % 64);
    if (syscall(SYS_mbind, addr, len, MPOL_BIND, mask, 65UL, 0U) != 0)
        perror("warning: mbind (falling back to first-touch placement)");
}

/* Kernel node holding the page at addr, or -1 if unknown. */
static int page_node(const void *addr)
{
    void *pg = (void *)((uintptr_t)addr & ~(uintptr_t)(page_size - 1));
    int status = -1;
    if (syscall(SYS_move_pages, 0, 1UL, &pg, NULL, &status, 0) != 0)
        return -1;
    return status;
}

/* ---------- set-up phases ---------- */

struct setup_arg {
    int k;
    int nwarm;
};

/* Fault the file mapping in; chunk c goes to node (c % nwarm). */
static void *warm_shared(void *p)
{
    struct setup_arg *a = p;
    volatile unsigned long sink = 0;
    pin_to_node(a->k);
    for (size_t off = 0, c = 0; off < file_size; off += CHUNK, c++) {
        if ((int)(c % a->nwarm) != a->k)
            continue;
        size_t end = off + CHUNK < file_size ? off + CHUNK : file_size;
        for (size_t o = off; o < end; o += page_size)
            sink += main_map[o];
    }
    return NULL;
}

/* Fill region[k] from the file, running on node k. */
static void *fill_region(void *p)
{
    struct setup_arg *a = p;
    pin_to_node(a->k);
    for (size_t off = 0; off < file_size;) {
        size_t want = file_size - off < CHUNK ? file_size - off : CHUNK;
        ssize_t got = pread(fd, region[a->k] + off, want, off);
        if (got <= 0)
            die("pread");
        off += got;
    }
    return NULL;
}

/* filerep: socket k faults in its replica, and its share of the shared file
 * beyond the replicated part (2 MiB chunks, round robin over the sockets). */
static void *warm_filerep(void *p)
{
    struct setup_arg *a = p;
    volatile unsigned long sink = 0;
    pin_to_node(a->k);
    for (size_t o = 0; o < nrep_blocks * block; o += page_size)
        sink += region[a->k][o];
    if (main_map)
        for (size_t off = nrep_blocks * block, c = 0; off < file_size; off += CHUNK, c++) {
            if ((int)(c % a->nwarm) != a->k)
                continue;
            size_t end = off + CHUNK < file_size ? off + CHUNK : file_size;
            for (size_t o = off; o < end; o += page_size)
                sink += main_map[o];
        }
    return NULL;
}

static void run_setup_threads(void *(*fn)(void *), int first, int count)
{
    pthread_t th[MAX_NODES];
    struct setup_arg args[MAX_NODES];
    for (int k = 0; k < count; k++) {
        args[k].k = first + k;
        args[k].nwarm = count;
        if (pthread_create(&th[k], NULL, fn, &args[k]) != 0)
            die("pthread_create");
    }
    for (int k = 0; k < count; k++)
        pthread_join(th[k], NULL);
}

static unsigned char *anon_region(int k_bind)
{
    unsigned char *r = mmap(NULL, file_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED)
        die("mmap anonymous (not enough memory?)");
    madvise(r, file_size, MADV_NOHUGEPAGE); /* 4 KiB pages, like the file */
    if (k_bind >= 0)
        bind_to_node(r, file_size, k_bind);
    return r;
}

static _Atomic unsigned long *version_array(int k_bind)
{
    size_t len = nblocks * sizeof(unsigned long);
    _Atomic unsigned long *v = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (v == MAP_FAILED)
        die("mmap version array");
    if (k_bind >= 0)
        bind_to_node((void *)v, len, k_bind);
    for (size_t i = 0; i < nblocks; i++)
        atomic_init(&v[i], 2UL); /* even = stable; all copies start equal */
    return v;
}

/* ---------- placement-based locality (pinned, evenly spread threads) ---------- */

static double fraction_on_node(unsigned char *base, int want, long *dist)
{
    static void *pages[SAMPLES];
    static int status[SAMPLES];
    uint64_t x = 88172645463325252ULL;
    size_t npages = file_size / page_size;
    long valid = 0, hit = 0;

    if (fake_nodes > 0 || npages == 0)
        return -1;
    for (int i = 0; i < SAMPLES; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        pages[i] = base + (x % npages) * page_size;
        status[i] = -1;
    }
    if (syscall(SYS_move_pages, 0, (unsigned long)SAMPLES, pages, NULL, status, 0) != 0)
        return -1;
    for (int i = 0; i < SAMPLES; i++) {
        if (status[i] < 0)
            continue;
        valid++;
        if (status[i] == want)
            hit++;
        if (dist && status[i] < MAX_NODES)
            dist[status[i]]++;
    }
    return valid ? (double)hit / valid : -1;
}

static double local_ratio(const int *threads_on_node, int nthreads)
{
    double sum = 0;
    for (int k = 0; k < nnodes; k++) {
        if (threads_on_node[k] == 0)
            continue;
        double f = fraction_on_node(region[k], nodes[k].id, NULL);
        if (f < 0)
            return -1;
        sum += f * threads_on_node[k];
    }
    return sum / nthreads;
}

/* ---------- main/twin protocol (replica mode with writes) ---------- */

struct worker {
    int k;                /* node index it is pinned to (or starts on) */
    uint64_t seed;
    uint64_t ops, reads, writes;
    uint64_t refreshes, retries, check_errors, node_changes;
    uint64_t samples, samples_local;
    unsigned long sink;
    _Atomic uint64_t live_ops;    /* published every 64 operations */
};

static void stamp(unsigned char *buf, unsigned long version)
{
    uint64_t w = ((uint64_t)MAGIC << 32) | (version & 0xffffffffUL);
    uint64_t *p = (uint64_t *)buf;
    for (size_t i = 0; i < block / 8; i++)
        p[i] = w;
}

static void verify(struct worker *w, const unsigned char *buf)
{
    const uint64_t *p = (const uint64_t *)buf;
    if ((p[0] >> 32) != MAGIC)
        return; /* never written in this run: original file content */
    for (size_t i = 1; i < block / 8; i++)
        if (p[i] != p[0]) {
            w->check_errors++;
            return;
        }
}

static const unsigned char *rep_write(struct worker *w, size_t blk, unsigned char *src)
{
    _Atomic unsigned long *v = &ver_main[blk];
    unsigned long cur;
    for (;;) {
        cur = atomic_load_explicit(v, memory_order_acquire);
        if (cur & 1) {
            cpu_relax();
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(v, &cur, cur + 1,
                memory_order_acq_rel, memory_order_relaxed))
            break;
    }
    if (check)
        stamp(src, cur + 2);
    unsigned char *dst = main_map + blk * block;
    memcpy(dst, src, block);
    atomic_store_explicit(v, cur + 2, memory_order_release);
    (void)w;
    return dst;
}

static const unsigned char *rep_read(struct worker *w, int k, size_t blk, unsigned char *dst)
{
    _Atomic unsigned long *rv = &ver_rep[k][blk];
    _Atomic unsigned long *mv = &ver_main[blk];
    unsigned char *twin = region[k] + blk * block;
    unsigned char *mainp = main_map + blk * block;

    for (;;) {
        unsigned long r = atomic_load_explicit(rv, memory_order_acquire);
        unsigned long m = atomic_load_explicit(mv, memory_order_acquire);
        if (r == m) {
            /* local twin is current */
            memcpy(dst, twin, block);
            atomic_thread_fence(memory_order_acquire);
            if (atomic_load_explicit(rv, memory_order_relaxed) == r)
                return twin;
            w->retries++;
            continue;
        }
        /* twin stale or being refreshed: read the main copy (seqlock) */
        unsigned long m1;
        for (;;) {
            m1 = atomic_load_explicit(mv, memory_order_acquire);
            if (m1 & 1) {
                cpu_relax();
                continue;
            }
            memcpy(dst, mainp, block);
            atomic_thread_fence(memory_order_acquire);
            if (atomic_load_explicit(mv, memory_order_relaxed) == m1)
                break;
            w->retries++;
        }
        /* replicate on read: refresh the local twin from the copy just read */
        if (r != BUSY && atomic_compare_exchange_strong_explicit(rv, &r, BUSY,
                memory_order_acq_rel, memory_order_relaxed)) {
            memcpy(twin, dst, block);
            atomic_store_explicit(rv, m1, memory_order_release);
            w->refreshes++;
        }
        return mainp;
    }
}

/* ---------- measured workload ---------- */

static void *worker_main(void *p)
{
    struct worker *w = p;
    unsigned char *buf;
    uint64_t x = w->seed, ops = 0, reads = 0, writes = 0, since = 0;
    unsigned long sink = 0;
    const unsigned char *last = NULL;
    int cur;

    if (!unpinned)
        pin_to_node(w->k);
    cur = unpinned ? current_node() : w->k;
    if (posix_memalign((void **)&buf, 4096, block) != 0)
        die("posix_memalign");
    memset(buf, 0, block);
    pthread_barrier_wait(&start_barrier);

    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        unsigned char *base = (mode == M_REPLICA || mode == M_FILEREP) ? region[cur] : region[0];
        for (int i = 0; i < 64; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            size_t blk;
            if (hot_blocks) {
                uint64_t y = x * 0x2545F4914F6CDD1DULL;
                blk = ((int)((y >> 33) % 100) < hot_access)
                    ? x % hot_blocks : hot_blocks + x % (nblocks - hot_blocks);
            } else {
                blk = x % nblocks;
            }
            int is_read = read_pct >= 100 || (int)((x >> 40) % 100) < read_pct;
            if (rw_replica) {
                if (is_read) {
                    last = rep_read(w, cur, blk, buf);
                    if (check)
                        verify(w, buf);
                    reads++;
                } else {
                    last = rep_write(w, blk, buf);
                    writes++;
                }
            } else {
                unsigned char *a = (mode == M_FILEREP && blk >= nrep_blocks ? main_map : base)
                                   + blk * block;
                if (is_read) {
                    memcpy(buf, a, block);
                    reads++;
                } else {
                    memcpy(a, buf, block);
                    writes++;
                }
                last = a;
            }
            sink += buf[x % block];
        }
        ops += 64;
        atomic_store_explicit(&w->live_ops, ops, memory_order_relaxed);
        if (unpinned) {
            int c = current_node();
            if (c != cur) {
                w->node_changes++;
                cur = c;
            }
        }
        if (sample_every > 0 && fake_nodes == 0) {
            since += 64;
            if (since >= (uint64_t)sample_every) {
                since = 0;
                int pn = page_node(last);
                if (pn >= 0) {
                    w->samples++;
                    if (pn == nodes[current_node()].id)
                        w->samples_local++;
                }
            }
        }
    }
    w->ops = ops;
    w->reads = reads;
    w->writes = writes;
    w->sink = sink;
    free(buf);
    return NULL;
}

static int read_numa_balancing(void)
{
    int v = -1;
    FILE *f = fopen("/proc/sys/kernel/numa_balancing", "r");
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

static long long read_io_bytes(void)
{
    long long v = -1;
    char line[256];
    FILE *f = fopen("/proc/self/io", "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "read_bytes: %lld", &v) == 1)
            break;
    fclose(f);
    return v;
}

static long major_faults(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_majflt;
}

/* The memory limit of this process's cgroup in GiB, or -1 if there is none. */
static double mem_limit_gib(void)
{
    char line[512], path[700];
    double gib = -1;
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "0::", 3) == 0) {
            line[strcspn(line, "\n")] = 0;
            snprintf(path, sizeof(path), "/sys/fs/cgroup%s/memory.max", line + 3);
            FILE *m = fopen(path, "r");
            if (m) {
                char v[64];
                if (fgets(v, sizeof(v), m) && strncmp(v, "max", 3) != 0)
                    gib = atof(v) / (1UL << 30);
                fclose(m);
            }
            break;
        }
    }
    fclose(f);
    return gib;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s -f FILE [-m shared|replica|anon] [-w spread|single] [-t THREADS]\n"
        "          [-d SECONDS] [-b BLOCK] [-r READ_PCT] [-u] [-n NODE] [-W] [-P] [-C]\n"
        "          [-S N] [-e] [-F N] [-H]\n"
        "  -m  shared  = one mmap of the file for all threads (baseline)\n"
        "      replica = one copy per NUMA node (with -r < 100: main/twin protocol)\n"
        "      anon    = anonymous memory filled on node 0 (NUMA balancing control)\n"
        "      filerep = evictable file replicas, one per node (-R PREFIX)\n"
        "  -w  shared mode: who loads the file, spread (default) or single\n"
        "  -r  share of operations that are reads, 0-100 (default 100)\n"
        "  -u  do not pin threads      -n NODE  pin every thread to that node\n"
        "  -W  map the file read-write  -C  check reads are never torn\n"
        "  -P  replica mode: main/twin protocol even at 100%% reads\n"
        "  -S  sample locality every N operations (default 4096, 0 = off)\n"
        "  -R  filerep: replica files are PREFIX0.bin, PREFIX1.bin, ...\n"
        "  -k  H:A  the first H%% of blocks get A%% of accesses   -A  random-access advice\n"
        "  -D  warm-up seconds before measuring\n"
        "  -e  evict the file from the page cache first\n"
        "  -F  testing only: pretend the machine has N NUMA nodes\n"
        "  -H  print the CSV header and exit\n", prog);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *mode_s = "shared", *warm = "spread";
    int nthreads = 0, evict = 0, opt;
    double seconds = 20;

    while ((opt = getopt(argc, argv, "f:m:w:t:d:b:r:un:WPCS:eF:HR:k:AD:")) != -1) {
        switch (opt) {
        case 'f': file_path = optarg; break;
        case 'm': mode_s = optarg; break;
        case 'w': warm = optarg; break;
        case 't': nthreads = atoi(optarg); break;
        case 'd': seconds = atof(optarg); break;
        case 'b': block = strtoul(optarg, NULL, 10); break;
        case 'r': read_pct = atoi(optarg); break;
        case 'u': unpinned = 1; break;
        case 'n': all_on = atoi(optarg); break;
        case 'W': writable = 1; break;
        case 'P': force_proto = 1; break;
        case 'C': check = 1; break;
        case 'S': sample_every = atol(optarg); break;
        case 'e': evict = 1; break;
        case 'R': rep_prefix = optarg; break;
        case 'k':
            if (sscanf(optarg, "%d:%d", &hot_pct, &hot_access) != 2 || hot_pct <= 0 ||
                hot_pct >= 100 || hot_access < 0 || hot_access > 100)
                usage(argv[0]);
            break;
        case 'A': rand_advice = 1; break;
        case 'D': warm_secs = atof(optarg); break;
        case 'F': fake_nodes = atoi(optarg); break;
        case 'H':
            printf("mode,warm,numa_balancing,threads,nodes,file_mb,block,seconds,"
                   "gib_per_s,mops_per_s,local_before,local_after,setup_s,"
                   "pinned,read_pct,writable,sampled_local,node_changes_per_thread_s,"
                   "refreshes_per_s,retries_per_s,check_errors,"
                   "hot_pct,hot_access_pct,rep_frac,disk_read_mb_s,majflt_per_s,mem_limit_gib\n");
            return 0;
        default: usage(argv[0]);
        }
    }
    if (strcmp(mode_s, "shared") == 0) mode = M_SHARED;
    else if (strcmp(mode_s, "replica") == 0) mode = M_REPLICA;
    else if (strcmp(mode_s, "anon") == 0) mode = M_ANON;
    else if (strcmp(mode_s, "filerep") == 0) mode = M_FILEREP;
    else usage(argv[0]);
    int single = strcmp(warm, "single") == 0;
    if (!file_path || (!single && strcmp(warm, "spread") != 0) || block < 64 ||
        block % 8 != 0 || fake_nodes < 0 || fake_nodes > MAX_NODES || seconds <= 0 ||
        read_pct < 0 || read_pct > 100 || (unpinned && all_on >= 0))
        usage(argv[0]);
    if (read_pct < 100 && (mode == M_ANON || mode == M_FILEREP)) {
        fprintf(stderr, "error: %s mode is read-only\n", mode_s);
        return 2;
    }
    if (mode == M_FILEREP && !rep_prefix) {
        fprintf(stderr, "error: filerep mode needs -R PREFIX\n");
        return 2;
    }
    rw_replica = (mode == M_REPLICA && (read_pct < 100 || force_proto));
    if (read_pct < 100 || rw_replica)
        writable = 1;
    if (check && !rw_replica)
        fprintf(stderr, "note: -C only has an effect in replica mode with writes\n");

    page_size = sysconf(_SC_PAGESIZE);
    discover_nodes();
    if (all_on >= nnodes) {
        fprintf(stderr, "error: -n %d but only %d node(s)\n", all_on, nnodes);
        return 2;
    }
    if (nthreads <= 0)
        nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nnodes == 1)
        fprintf(stderr, "warning: only one NUMA node found; there is no remote memory here.\n");

    fd = open(file_path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0)
        die(file_path);
    struct stat st;
    if (fstat(fd, &st) != 0)
        die("fstat");
    nblocks = (size_t)st.st_size / block;
    file_size = nblocks * block;
    if (nblocks == 0) {
        fprintf(stderr, "error: %s is smaller than one block\n", file_path);
        return 1;
    }
    if (hot_pct > 0) {
        hot_blocks = nblocks * hot_pct / 100;
        if (hot_blocks == 0 || hot_blocks >= nblocks)
            hot_blocks = 0;
    }
    if (evict && posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0)
        fprintf(stderr, "warning: could not evict the file from the page cache\n");

    /* ----- set-up ----- */
    double t0 = now_sec();
    if (mode == M_FILEREP) {
        nrep_blocks = nblocks;
        int rep_fd[MAX_NODES];
        for (int k = 0; k < nnodes; k++) {
            char path[1024];
            struct stat rs;
            snprintf(path, sizeof(path), "%s%d.bin", rep_prefix, k);
            rep_fd[k] = open(path, O_RDONLY);
            if (rep_fd[k] < 0 || fstat(rep_fd[k], &rs) != 0)
                die(path);
            if ((size_t)rs.st_size / block < nrep_blocks)
                nrep_blocks = (size_t)rs.st_size / block;
            if (evict)
                posix_fadvise(rep_fd[k], 0, 0, POSIX_FADV_DONTNEED);
        }
        if (nrep_blocks == 0) {
            fprintf(stderr, "error: replica files are empty\n");
            return 1;
        }
        for (int k = 0; k < nnodes; k++) {
            region[k] = mmap(NULL, nrep_blocks * block, PROT_READ, MAP_SHARED, rep_fd[k], 0);
            if (region[k] == MAP_FAILED)
                die("mmap replica file");
        }
        if (nrep_blocks < nblocks) {
            main_map = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
            if (main_map == MAP_FAILED)
                die("mmap file");
        }
        run_setup_threads(warm_filerep, 0, nnodes);
        if (rand_advice) {
            for (int k = 0; k < nnodes; k++)
                madvise(region[k], nrep_blocks * block, MADV_RANDOM);
            if (main_map)
                madvise(main_map, file_size, MADV_RANDOM);
        }
    } else if (mode == M_ANON) {
        region[0] = anon_region(-1);
        run_setup_threads(fill_region, 0, 1); /* first touch on node 0 */
        for (int k = 1; k < nnodes; k++)
            region[k] = region[0];
    } else if (mode == M_REPLICA && !rw_replica) {
        /* read-only replicas, exactly as in run 1 */
        for (int k = 0; k < nnodes; k++)
            region[k] = anon_region(k);
        run_setup_threads(fill_region, 0, nnodes);
        for (int k = 0; k < nnodes; k++)
            mprotect(region[k], file_size, PROT_READ);
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    } else {
        int prot = PROT_READ | (writable ? PROT_WRITE : 0);
        main_map = mmap(NULL, file_size, prot, MAP_SHARED, fd, 0);
        if (main_map == MAP_FAILED)
            die("mmap file");
        run_setup_threads(warm_shared, 0, single ? 1 : nnodes);
        if (rand_advice)
            madvise(main_map, file_size, MADV_RANDOM);
        if (rw_replica) {
            /* twins: one copy per node, plus their version arrays */
            ver_main = version_array(-1);
            for (int k = 0; k < nnodes; k++) {
                region[k] = anon_region(k);
                ver_rep[k] = version_array(k);
            }
            run_setup_threads(fill_region, 0, nnodes);
        } else {
            for (int k = 0; k < nnodes; k++)
                region[k] = main_map;
        }
    }
    double setup_s = now_sec() - t0;

    /* ----- placement-based locality (only meaningful for pinned, even spread) ----- */
    int threads_on_node[MAX_NODES] = {0};
    for (int i = 0; i < nthreads; i++)
        threads_on_node[all_on >= 0 ? all_on : i % nnodes]++;
    int even = !unpinned && all_on < 0 && !rw_replica && mode != M_FILEREP;
    double local_before = even ? local_ratio(threads_on_node, nthreads) : -1;
    if (mode == M_SHARED && fake_nodes == 0) {
        long dist[MAX_NODES] = {0}, total = 0;
        fraction_on_node(region[0], -1, dist);
        for (int n = 0; n < MAX_NODES; n++)
            total += dist[n];
        for (int n = 0; n < MAX_NODES && total; n++)
            if (dist[n])
                fprintf(stderr, "  file pages on node %d: %.1f%%\n", n, 100.0 * dist[n] / total);
    }

    /* ----- measured run ----- */
    pthread_t *th = calloc(nthreads, sizeof(*th));
    struct worker *ws = calloc(nthreads, sizeof(*ws));
    if (!th || !ws)
        die("calloc");
    pthread_barrier_init(&start_barrier, NULL, nthreads + 1);
    for (int i = 0; i < nthreads; i++) {
        ws[i].k = all_on >= 0 ? all_on : i % nnodes;
        ws[i].seed = 0x9E3779B97F4A7C15ULL * (uint64_t)(i + 1);
        if (pthread_create(&th[i], NULL, worker_main, &ws[i]) != 0)
            die("pthread_create");
    }
    pthread_barrier_wait(&start_barrier);
    if (warm_secs > 0) {
        struct timespec wt = { (time_t)warm_secs, (long)((warm_secs - (time_t)warm_secs) * 1e9) };
        nanosleep(&wt, NULL);
    }
    uint64_t ops0 = 0;
    for (int i = 0; i < nthreads; i++)
        ops0 += atomic_load_explicit(&ws[i].live_ops, memory_order_relaxed);
    long long io0 = read_io_bytes();
    long maj0 = major_faults();
    double t1 = now_sec();
    struct timespec ts = { (time_t)seconds, (long)((seconds - (time_t)seconds) * 1e9) };
    nanosleep(&ts, NULL);
    uint64_t ops1 = 0;
    for (int i = 0; i < nthreads; i++)
        ops1 += atomic_load_explicit(&ws[i].live_ops, memory_order_relaxed);
    long long io1 = read_io_bytes();
    long maj1 = major_faults();
    double elapsed = now_sec() - t1;
    atomic_store(&stop_flag, 1);
    uint64_t ops = ops1 - ops0, refreshes = 0, retries = 0, errors = 0, changes = 0;
    uint64_t samples = 0, samples_local = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
        refreshes += ws[i].refreshes;
        retries += ws[i].retries;
        errors += ws[i].check_errors;
        changes += ws[i].node_changes;
        samples += ws[i].samples;
        samples_local += ws[i].samples_local;
    }
    double local_after = even ? local_ratio(threads_on_node, nthreads) : -1;
    double sampled = samples ? (double)samples_local / samples : -1;

    double gib_s = (double)ops * block / elapsed / (1UL << 30);
    double mops_s = ops / elapsed / 1e6;
    double changes_s = (double)changes / nthreads / elapsed;
    int nb = read_numa_balancing();
    double multiplier = (mode == M_REPLICA) ? nnodes + (rw_replica ? 1 : 0) : 1;
    double rep_frac = mode == M_REPLICA ? 1.0 : 0.0;
    if (mode == M_FILEREP) {
        rep_frac = (double)nrep_blocks / nblocks;
        multiplier = nnodes * rep_frac + (nrep_blocks < nblocks ? 1.0 : 0.0);
    }
    double disk_mb_s = (io0 >= 0 && io1 >= 0) ? (io1 - io0) / elapsed / 1e6 : -1;
    double majflt_s = (maj1 - maj0) / elapsed;
    double limit = mem_limit_gib();

    fprintf(stderr, "%s%s%s%s: %d threads%s on %d node(s), r=%d%%, %.2f GiB/s, %.3f M ops/s",
            mode_s, mode == M_SHARED ? "/" : "", mode == M_SHARED ? warm : "",
            writable ? " (rw map)" : "", nthreads,
            unpinned ? " unpinned" : (all_on >= 0 ? " on one node" : ""),
            nnodes, read_pct, gib_s, mops_s);
    if (sampled >= 0)
        fprintf(stderr, ", %.1f%% local (sampled)", 100 * sampled);
    else if (local_after >= 0)
        fprintf(stderr, ", %.1f%% local", 100 * local_after);
    if (unpinned)
        fprintf(stderr, ", %.2f node changes/thread/s", changes_s);
    if (rw_replica)
        fprintf(stderr, ", %.2f M refreshes/s", refreshes / elapsed / 1e6);
    if (check)
        fprintf(stderr, ", check errors %llu", (unsigned long long)errors);
    if (mode == M_FILEREP)
        fprintf(stderr, ", %.0f%% replicated", 100 * rep_frac);
    if (disk_mb_s > 0.05)
        fprintf(stderr, ", disk reads %.1f MB/s", disk_mb_s);
    if (limit > 0)
        fprintf(stderr, ", memory limit %.1f GiB", limit);
    fprintf(stderr, ", set-up %.1f s, data memory %.0f MiB\n", setup_s,
            (double)file_size * multiplier / (1 << 20));

    printf("%s,%s,%d,%d,%d,%.0f,%zu,%.2f,%.4f,%.4f,%.4f,%.4f,%.2f,%s,%d,%d,%.4f,%.4f,%.4f,%.4f,%llu,"
           "%d,%d,%.4f,%.2f,%.1f,%.2f\n",
           mode_s, mode == M_SHARED ? warm : "-", nb, nthreads, nnodes,
           (double)file_size / (1 << 20), block, elapsed, gib_s, mops_s,
           local_before, local_after, setup_s,
           unpinned ? "no" : (all_on >= 0 ? "one-node" : "yes"),
           read_pct, writable, sampled, changes_s,
           refreshes / elapsed / 1e6, retries / elapsed / 1e6,
           (unsigned long long)errors,
           hot_blocks ? hot_pct : 0, hot_blocks ? hot_access : 0, rep_frac,
           disk_mb_s, majflt_s, limit);
    return 0;
}

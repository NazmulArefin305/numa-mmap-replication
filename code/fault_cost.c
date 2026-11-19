/*
 * fault_cost.c
 *
 * Estimates what PaCaR's proposed transparent mmap design would add to every
 * write: the page is write-protected, the write faults, the fault handler
 * unprotects the page, and the write goes ahead.
 *
 * It times, per 4 KiB page:
 *   plain       a write to an ordinary writable page
 *   protected   a write to a write-protected page (signal + mprotect + retry)
 *   reprotect   mprotect() making one page read-only again
 *
 * With -t N, N extra threads keep running (spread over all CPUs) and reading
 * the same memory. Then every mprotect() must flush the TLB on their CPUs too,
 * which is what happens in a real multi-threaded program.
 *
 * Build:  make
 * Run:    ./fault_cost            (no extra threads)
 *         ./fault_cost -t 39      (39 extra threads)
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static long ps;
static size_t npages = 16384; /* 64 MiB */
static unsigned char *mem;
static atomic_int stop;
static atomic_long faults;

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)ctx;
    uintptr_t a = (uintptr_t)si->si_addr & ~(uintptr_t)(ps - 1);
    if ((unsigned char *)a < mem || (unsigned char *)a >= mem + npages * ps)
        _exit(3); /* a real crash, not one of ours */
    mprotect((void *)a, ps, PROT_READ | PROT_WRITE);
    atomic_fetch_add_explicit(&faults, 1, memory_order_relaxed);
}

static void *background(void *p)
{
    volatile unsigned long sink = 0;
    size_t i = (size_t)(uintptr_t)p * 977;
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        sink += mem[(i % npages) * ps];
        i += 7;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int extra = 0, opt, rounds = 5;
    while ((opt = getopt(argc, argv, "t:p:r:")) != -1) {
        switch (opt) {
        case 't': extra = atoi(optarg); break;
        case 'p': npages = strtoul(optarg, NULL, 10); break;
        case 'r': rounds = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-t EXTRA_THREADS] [-p PAGES] [-r ROUNDS]\n", argv[0]);
            return 2;
        }
    }
    ps = sysconf(_SC_PAGESIZE);
    mem = mmap(NULL, npages * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    madvise(mem, npages * ps, MADV_NOHUGEPAGE);
    memset(mem, 1, npages * ps); /* fault every page in first */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    pthread_t *th = calloc(extra > 0 ? extra : 1, sizeof(*th));
    for (int i = 0; i < extra; i++)
        pthread_create(&th[i], NULL, background, (void *)(uintptr_t)(i + 1));
    usleep(100000);

    double plain = 1e300, prot = 1e300, reprot = 1e300;
    for (int r = 0; r < rounds; r++) {
        double t0 = now_ns();
        for (size_t i = 0; i < npages; i++)
            ((volatile unsigned char *)mem)[i * ps] = (unsigned char)r;
        double t1 = now_ns();
        if ((t1 - t0) / npages < plain)
            plain = (t1 - t0) / npages;

        t0 = now_ns();
        for (size_t i = 0; i < npages; i++)
            mprotect(mem + i * ps, ps, PROT_READ);
        t1 = now_ns();
        if ((t1 - t0) / npages < reprot)
            reprot = (t1 - t0) / npages;

        long f0 = atomic_load(&faults);
        t0 = now_ns();
        for (size_t i = 0; i < npages; i++)
            ((volatile unsigned char *)mem)[i * ps] = (unsigned char)(r + 1);
        t1 = now_ns();
        if (atomic_load(&faults) - f0 != (long)npages)
            fprintf(stderr, "warning: expected %zu faults, got %ld\n", npages, atomic_load(&faults) - f0);
        if ((t1 - t0) / npages < prot)
            prot = (t1 - t0) / npages;
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < extra; i++)
        pthread_join(th[i], NULL);

    printf("extra_threads,plain_write_ns,protected_write_ns,reprotect_ns,fault_overhead_ns\n");
    printf("%d,%.1f,%.1f,%.1f,%.1f\n", extra, plain, prot, reprot, prot - plain);
    fprintf(stderr, "%d extra threads: plain write %.0f ns, write to a protected page %.0f ns "
            "(+%.0f ns), re-protecting a page %.0f ns  [best of %d rounds, %zu pages each]\n",
            extra, plain, prot, prot - plain, reprot, rounds, npages);
    return 0;
}

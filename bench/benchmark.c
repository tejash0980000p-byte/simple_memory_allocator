/*
 * benchmark.c - Compare first/next/best/worst-fit against the standard libc malloc.
 *
 * Three workloads (identical random sequence for every allocator -> fair test):
 *   1. sequential : 20,000 equal blocks allocated, then freed in order
 *   2. churn      : 100,000 random allocate/free operations, sizes 16..1024 B
 *   3. phased     : repeated waves of small + large allocations with random frees
 *                   (this one is designed to cause FRAGMENTATION)
 *
 * Metrics: time, failed allocations, peak usage, free blocks, external
 * fragmentation %, largest free block.  Results are also saved in results.csv
 *
 * Run:  make bench
 */
#include "../src/allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void *(*alloc_fn)(size_t);
typedef void  (*free_fn)(void *);

static void *sys_alloc(size_t n) { return malloc(n); }
static void  sys_free(void *p)   { free(p); }

/* ---- deterministic RNG so every allocator sees the same request stream ---- */
static uint64_t rng_state;
static void     rng_seed(uint64_t s) { rng_state = s ? s : 1; }
static uint32_t rnd(void)
{
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 2685821657736338717ULL) >> 32);
}

static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static alloc_stats_t snap;
static int           have_snap;
static size_t        fails;

static void take_snapshot(int mine) { if (mine) { get_stats(&snap); have_snap = 1; } }

static void touch(void *p, size_t n) { memset(p, 0x5A, n < 64 ? n : 64); }

static void wl_sequential(alloc_fn A, free_fn F, int mine)
{
    enum { N = 20000 };
    void **v = malloc(N * sizeof *v);
    for (int i = 0; i < N; i++) { v[i] = A(64); if (v[i]) touch(v[i], 64); else fails++; }
    take_snapshot(mine);
    for (int i = 0; i < N; i++) F(v[i]);
    free(v);
}

static void wl_churn(alloc_fn A, free_fn F, int mine)
{
    enum { SLOTS = 2000, OPS = 100000 };
    void **s = calloc(SLOTS, sizeof *s);
    rng_seed(42);
    for (int i = 0; i < OPS; i++) {
        uint32_t k = rnd() % SLOTS;
        if (s[k]) { F(s[k]); s[k] = NULL; }
        else {
            size_t n = 16 + rnd() % 1009;
            s[k] = A(n);
            if (s[k]) touch(s[k], n); else fails++;
        }
    }
    take_snapshot(mine);
    for (int i = 0; i < SLOTS; i++) if (s[i]) F(s[i]);
    free(s);
}

static void free_fraction(void **live, size_t *n, int pct, free_fn F)
{
    for (size_t i = *n; i-- > 0;)
        if ((int)(rnd() % 100) < pct) { F(live[i]); live[i] = live[--*n]; }
}

static void wl_phased(alloc_fn A, free_fn F, int mine)
{
    enum { ROUNDS = 40, MAXLIVE = 200000 };
    void **live = malloc(MAXLIVE * sizeof *live);
    size_t n = 0;
    rng_seed(7);
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < 400; i++) {
            size_t sz = 16 + rnd() % 496;
            void *p = A(sz);
            if (p) { touch(p, sz); live[n++] = p; } else fails++;
        }
        free_fraction(live, &n, 55, F);
        for (int i = 0; i < 60; i++) {
            size_t sz = 1024 + rnd() % 3072;
            void *p = A(sz);
            if (p) { touch(p, sz); live[n++] = p; } else fails++;
        }
        free_fraction(live, &n, 35, F);
    }
    take_snapshot(mine);
    for (size_t i = 0; i < n; i++) F(live[i]);
    free(live);
}

typedef struct { const char *name; void (*run)(alloc_fn, free_fn, int); size_t heap; } workload_t;

int main(void)
{
    const workload_t W[] = {
        { "sequential", wl_sequential, 4u << 20 },
        { "churn",      wl_churn,      8u << 20 },
        { "phased",     wl_phased,     2u << 20 },
    };
    const fit_strategy_t S[] = { FIRST_FIT, NEXT_FIT, BEST_FIT, WORST_FIT };
    enum { REPS = 3 };

    FILE *csv = fopen("results.csv", "w");
    if (csv) fprintf(csv, "workload,allocator,time_ms,failed,peak_kb,free_blocks,ext_frag_pct,largest_free_kb\n");

    printf("==================== Allocator benchmark ====================\n");
    for (size_t w = 0; w < sizeof W / sizeof W[0]; w++) {
        printf("\nWorkload: %-10s (heap = %zu KB, best of %d runs)\n", W[w].name, W[w].heap >> 10, REPS);
        printf("  %-12s %9s %8s %10s %11s %9s %12s\n",
               "allocator", "time(ms)", "failed", "peak(KB)", "free blks", "frag(%)", "largest(KB)");
        printf("  %-12s %9s %8s %10s %11s %9s %12s\n",
               "------------", "---------", "------", "--------", "---------", "-------", "-----------");

        for (int s = 0; s < 4; s++) {
            double best = 1e18;
            for (int rep = 0; rep < REPS; rep++) {
                allocator_init(W[w].heap);
                allocator_set_verbose(0);
                set_strategy(S[s]);
                fails = 0; have_snap = 0;
                double t0 = now_ms();
                W[w].run(my_malloc, my_free, 1);
                double dt = now_ms() - t0;
                if (dt < best) best = dt;
            }
            printf("  %-12s %9.2f %8zu %10zu %11zu %9.1f %12zu\n", strategy_name(S[s]), best, fails,
                   snap.peak_used >> 10, snap.free_blocks, snap.external_frag, snap.largest_free >> 10);
            if (csv) fprintf(csv, "%s,%s,%.3f,%zu,%zu,%zu,%.2f,%zu\n", W[w].name, strategy_name(S[s]), best,
                             fails, snap.peak_used >> 10, snap.free_blocks, snap.external_frag, snap.largest_free >> 10);
        }

        double best = 1e18;
        for (int rep = 0; rep < REPS; rep++) {
            fails = 0;
            double t0 = now_ms();
            W[w].run(sys_alloc, sys_free, 0);
            double dt = now_ms() - t0;
            if (dt < best) best = dt;
        }
        printf("  %-12s %9.2f %8zu %10s %11s %9s %12s\n", "libc malloc", best, fails, "-", "-", "-", "-");
        if (csv) fprintf(csv, "%s,libc malloc,%.3f,%zu,0,0,0,0\n", W[w].name, best, fails);
    }

    printf("\nHow to read this:\n"
           "  failed  = requests that returned NULL although memory may have been free (fragmentation!)\n"
           "  frag %%  = 100 * (1 - largest_free_block / total_free_bytes) at the end of the workload\n"
           "  Our allocator walks a free list (cost grows with the number of FREE blocks); libc uses\n"
           "  size-class bins + thread caches, so it stays faster when the heap is badly fragmented.\n"
           "  worst-fit is slowest AND most fragmented: it must scan every free block and shatters big holes.\n");
    if (csv) { fclose(csv); printf("\nSaved: results.csv   (run 'make plot' for charts)\n"); }
    allocator_destroy();
    return 0;
}

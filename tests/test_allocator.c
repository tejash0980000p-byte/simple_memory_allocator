/*
 * test_allocator.c - Correctness tests for the custom allocator.
 * Run:  make test        (exit code 0 = all passed)
 */
#include "../src/allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0, failed = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) { passed++; printf("  [PASS] %s\n", msg); }                \
        else      { failed++; printf("  [FAIL] %s   (line %d)\n", msg, __LINE__); } \
    } while (0)

static const fit_strategy_t ALL[] = { FIRST_FIT, NEXT_FIT, BEST_FIT, WORST_FIT };

static int filled_with(const unsigned char *p, size_t n, unsigned char v)
{
    for (size_t i = 0; i < n; i++) if (p[i] != v) return 0;
    return 1;
}

/* ---------------------------------------------------------------------- */
static void test_basic(void)
{
    printf("\n== Basic allocation, alignment, data integrity ==\n");
    allocator_init(64 * 1024);
    void *a = my_malloc(10), *b = my_malloc(100), *c = my_malloc(1);
    CHECK(a && b && c, "three allocations succeed");
    CHECK(((uintptr_t)a % 16 == 0) && ((uintptr_t)b % 16 == 0) && ((uintptr_t)c % 16 == 0),
          "all pointers are 16-byte aligned");
    memset(a, 0xAA, 10); memset(b, 0xBB, 100); memset(c, 0xCC, 1);
    CHECK(filled_with(a, 10, 0xAA) && filled_with(b, 100, 0xBB) && filled_with(c, 1, 0xCC),
          "blocks do not overlap (data intact)");
    CHECK(my_malloc(0) == NULL, "malloc(0) returns NULL");
    CHECK(my_usable_size(b) >= 100, "usable size >= requested size");
    CHECK(check_heap() == 0, "heap invariants hold");
}

static void test_split_and_reuse(void)
{
    printf("\n== Splitting and reuse of freed memory ==\n");
    allocator_init(64 * 1024);
    set_strategy(FIRST_FIT);
    alloc_stats_t s;
    get_stats(&s);
    CHECK(s.free_blocks == 1, "fresh heap = one big free block");
    void *a = my_malloc(100);
    get_stats(&s);
    CHECK(s.used_blocks == 1 && s.free_blocks == 1, "allocation splits the big block in two");
    my_free(a);
    void *b = my_malloc(100);
    CHECK(a == b, "freed block is reused for the same-size request");
    CHECK(check_heap() == 0, "heap invariants hold");
}

static void test_coalescing(void)
{
    printf("\n== Coalescing of neighbouring free blocks ==\n");
    const char *orders[] = { "forward", "backward", "middle-first" };
    for (int o = 0; o < 3; o++) {
        allocator_init(64 * 1024);
        void *p[3] = { my_malloc(200), my_malloc(300), my_malloc(400) };
        int seq[3][3] = { {0,1,2}, {2,1,0}, {1,0,2} };
        for (int i = 0; i < 3; i++) my_free(p[seq[o][i]]);
        alloc_stats_t s; get_stats(&s);
        char msg[96];
        snprintf(msg, sizeof msg, "free order %-12s -> heap collapses to ONE free block", orders[o]);
        CHECK(s.free_blocks == 1 && s.used_blocks == 0, msg);
        void *big = my_malloc(s.largest_free);
        CHECK(big != NULL, "whole heap usable again after coalescing");
    }
}

static void test_errors(void)
{
    printf("\n== Error detection (double free, invalid pointer, OOM) ==\n");
    allocator_init(64 * 1024);
    allocator_set_verbose(0);                   /* keep test output clean */
    alloc_stats_t s;

    void *a = my_malloc(64);
    my_free(a);
    my_free(a);                                 /* double free */
    get_stats(&s);
    CHECK(s.errors_detected == 1, "double free is detected and ignored");

    int local = 5;
    my_free(&local);                            /* pointer not from our heap */
    get_stats(&s);
    CHECK(s.errors_detected == 2, "free() of a foreign pointer is detected");

    my_free(NULL);
    CHECK(1, "free(NULL) is a harmless no-op");

    CHECK(my_malloc(10u * 1024 * 1024) == NULL, "request larger than heap returns NULL");
    CHECK(my_malloc((size_t)-1) == NULL, "huge size_t request does not overflow");
    CHECK(my_calloc((size_t)-1, 16) == NULL, "calloc multiplication overflow caught");
    CHECK(check_heap() == 0, "heap still healthy after misuse");

    /* heap exhaustion */
    allocator_init(4096);
    int n = 0; while (my_malloc(100)) n++;
    CHECK(n > 10, "allocator hands out blocks until the heap is full");
    CHECK(my_malloc(100) == NULL, "returns NULL (not crash) when out of memory");

    /* corruption detection */
    allocator_init(4096);
    void *x = my_malloc(64); my_malloc(64);
    allocator_inject_overflow(x);
    CHECK(check_heap() > 0, "buffer overflow into next header is detected");
    CHECK(my_malloc(16) == NULL, "corrupted heap refuses further allocations");
    allocator_set_verbose(1);
}

static void test_calloc_realloc(void)
{
    printf("\n== calloc / realloc ==\n");
    allocator_init(64 * 1024);
    unsigned char *z = my_calloc(50, 4);
    CHECK(z && filled_with(z, 200, 0), "calloc returns zeroed memory");

    unsigned char *p = my_malloc(64);
    memset(p, 0x5A, 64);
    unsigned char *q = my_realloc(p, 5000);     /* grow */
    CHECK(q && filled_with(q, 64, 0x5A), "realloc grow preserves old data");
    q = my_realloc(q, 32);                      /* shrink */
    CHECK(q && filled_with(q, 32, 0x5A), "realloc shrink preserves data");
    CHECK(check_heap() == 0, "heap invariants hold after realloc");

    unsigned char *r = my_realloc(NULL, 40);
    CHECK(r != NULL, "realloc(NULL, n) behaves like malloc");
    CHECK(my_realloc(r, 0) == NULL, "realloc(p, 0) behaves like free");

    /* in-place growth when the following block is free */
    allocator_init(64 * 1024);
    void *a = my_malloc(100);
    void *a2 = my_realloc(a, 400);
    CHECK(a == a2, "realloc grows in place when next block is free");
    CHECK(check_heap() == 0, "heap invariants hold");
}

/* Build: A(hole) sep B(hole) sep C(hole) sep D(hole) sep filler. Free the 4 holes.
 * Hole payload sizes: A=112, B=304, C=160, D=512.  Then request 140 bytes (->144).
 * first-fit -> B ; best-fit -> C ; worst-fit -> D                               */
static void test_strategies(void)
{
    printf("\n== Placement strategies behave as defined ==\n");
    void *hole[4], *res[4];
    size_t sz[4] = { 100, 300, 150, 500 };

    for (int s = 0; s < 4; s++) {
        allocator_init(4096);
        set_strategy(ALL[s]);
        for (int i = 0; i < 4; i++) { hole[i] = my_malloc(sz[i]); my_malloc(16); }
        alloc_stats_t st; get_stats(&st);
        my_malloc(st.largest_free);             /* filler: eat the tail */
        for (int i = 0; i < 4; i++) my_free(hole[i]);
        res[s] = my_malloc(140);

        char msg[96];
        if (ALL[s] == FIRST_FIT || ALL[s] == NEXT_FIT) {
            snprintf(msg, sizeof msg, "%-9s picks the FIRST hole that fits (300-byte hole)", strategy_name(ALL[s]));
            CHECK(res[s] == hole[1], msg);
        } else if (ALL[s] == BEST_FIT) {
            CHECK(res[s] == hole[2], "best-fit  picks the TIGHTEST hole (150-byte hole)");
        } else {
            CHECK(res[s] == hole[3], "worst-fit picks the LARGEST hole (500-byte hole)");
        }

        if (ALL[s] == NEXT_FIT) {               /* resumes after last allocation */
            void *n = my_malloc(100);           /* first-fit would use hole[0] */
            CHECK(n != hole[0] && (uintptr_t)n > (uintptr_t)hole[1],
                  "next-fit resumes searching after the previous allocation");
        }
        CHECK(check_heap() == 0, "heap invariants hold");
    }
}

/* Randomised stress test: thousands of malloc/realloc/free calls, each block
 * filled with a pattern that is verified before it is freed. */
typedef struct { unsigned char *p; size_t n; unsigned char v; } slot_t;

static void test_stress(void)
{
    printf("\n== Randomised stress test (all strategies) ==\n");
    enum { SLOTS = 200, OPS = 30000 };

    for (int s = 0; s < 4; s++) {
        allocator_init(1 << 20);
        set_strategy(ALL[s]);
        slot_t slot[SLOTS] = {{0}};
        srand(1234 + s);
        int ok = 1, bad_check = 0;

        for (int op = 0; op < OPS && ok; op++) {
            slot_t *t = &slot[rand() % SLOTS];
            int r = rand() % 10;
            if (!t->p) {                                    /* allocate */
                size_t n = 1 + (size_t)(rand() % 2000);
                t->p = my_malloc(n);
                if (t->p) { t->n = n; t->v = (unsigned char)(rand() & 0xFF); memset(t->p, t->v, n); }
            } else if (r < 6) {                             /* free */
                if (!filled_with(t->p, t->n, t->v)) ok = 0;
                my_free(t->p); t->p = NULL;
            } else {                                        /* realloc */
                if (!filled_with(t->p, t->n, t->v)) ok = 0;
                size_t n = 1 + (size_t)(rand() % 2000);
                unsigned char *q = my_realloc(t->p, n);
                if (q) {
                    size_t keep = n < t->n ? n : t->n;
                    if (!filled_with(q, keep, t->v)) ok = 0;
                    t->p = q; t->n = n; t->v = (unsigned char)(rand() & 0xFF); memset(q, t->v, n);
                }
            }
            if (op < 3000 && check_heap() != 0) { bad_check = 1; break; }
        }
        for (int i = 0; i < SLOTS; i++) if (slot[i].p) my_free(slot[i].p);

        alloc_stats_t st; get_stats(&st);
        char msg[96];
        snprintf(msg, sizeof msg, "%-9s: 30000 random ops, no data corruption", strategy_name(ALL[s]));
        CHECK(ok && !bad_check, msg);
        CHECK(st.free_blocks == 1 && st.used_blocks == 0 && check_heap() == 0,
              "after freeing everything the heap is ONE free block again");
    }
}

int main(void)
{
    printf("=========== Custom Memory Allocator - Test Suite ===========\n");
    test_basic();
    test_split_and_reuse();
    test_coalescing();
    test_errors();
    test_calloc_realloc();
    test_strategies();
    test_stress();
    allocator_destroy();

    printf("\n=============================================================\n");
    printf(" RESULT: %d passed, %d failed\n", passed, failed);
    printf("=============================================================\n");
    return failed ? 1 : 0;
}

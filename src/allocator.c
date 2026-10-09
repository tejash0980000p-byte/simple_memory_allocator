/*
 * allocator.c - Custom memory allocator: my_malloc / my_free / my_calloc / my_realloc
 *
 * HOW IT WORKS (summary)
 * ----------------------
 * 1. At start we grab one big contiguous "arena" (the heap) from the OS.
 * 2. The arena is carved into BLOCKS. Every block = [ header | payload ].
 * 3. All blocks (free AND used) sit in one doubly linked list in address order
 *    (needed for coalescing). On top of that, the FREE blocks are also chained in
 *    a second, address-ordered free list whose links live INSIDE the free
 *    block's own payload (zero extra header cost). malloc only walks the free
 *    list, so it never wastes time stepping over used blocks.
 * 4. my_malloc : find a free block (first/next/best/worst fit), SPLIT it if it
 *                is much bigger than needed, mark it used.
 * 5. my_free   : mark block free, then COALESCE with free neighbours so that
 *                small holes merge back into large ones (fights fragmentation).
 * 6. Safety    : magic numbers in each header detect double free, invalid free
 *                and heap corruption (buffer overflow into a neighbour header).
 *
 *   arena:  | hdr |  payload  | hdr | payload | hdr |     payload      |
 *             ^ block 0         ^ block 1       ^ block 2 (free)
 */
#include "allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGNMENT   16u
#define ALIGN_UP(x) (((x) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))
#define MAGIC_USED  0xA110C8EDu
#define MAGIC_FREE  0xF4EEB10Cu
#define DEFAULT_HEAP (1u << 20)               /* 1 MiB if user never calls init */

/* Block header. 32 bytes on 64-bit => payload stays 16-byte aligned. */
typedef struct block {
    size_t        size;      /* payload size in bytes (multiple of ALIGNMENT) */
    struct block *next;      /* next block in ADDRESS order                   */
    struct block *prev;      /* previous block in address order               */
    uint32_t      magic;     /* MAGIC_USED / MAGIC_FREE: integrity check      */
    uint32_t      is_free;
} block_t;

_Static_assert(sizeof(block_t) % ALIGNMENT == 0, "header must keep payload aligned");

#define HEADER_SIZE  (sizeof(block_t))
#define MIN_SPLIT    (HEADER_SIZE + ALIGNMENT) /* don't create uselessly tiny blocks */

/* A FREE block stores its free-list links in its (otherwise unused) payload. */
typedef struct { block_t *prev, *next; } flinks_t;
#define FL(b) ((flinks_t *)((unsigned char *)(b) + HEADER_SIZE))
_Static_assert(sizeof(flinks_t) <= ALIGNMENT, "a minimal block must be able to hold the free-list links");

/* ------------------------------ global state ------------------------------ */
static unsigned char *arena      = NULL;
static size_t         arena_size = 0;
static block_t       *head       = NULL;        /* first block (address order)   */
static block_t       *free_head  = NULL;        /* first FREE block              */
static block_t       *rover      = NULL;        /* NEXT_FIT resume point (a free block, or NULL) */
static fit_strategy_t strategy   = FIRST_FIT;
static int            verbose    = 1;
static int            corrupted  = 0;           /* set when check_heap fails     */

static size_t cur_used, peak_used;
static size_t n_alloc, n_free, n_fail, n_errors;

static void diag(const char *fmt, const char *arg)
{
    if (verbose) { fprintf(stderr, "[allocator] "); fprintf(stderr, fmt, arg); fputc('\n', stderr); }
}

/* ------------------------------- lifecycle -------------------------------- */
int allocator_init(size_t heap_size)
{
    if (arena) allocator_destroy();
    heap_size = ALIGN_UP(heap_size);
    if (heap_size < HEADER_SIZE + ALIGNMENT) return -1;

    arena = aligned_alloc(ALIGNMENT, heap_size);
    if (!arena) return -1;

    arena_size = heap_size;
    head = (block_t *)arena;                    /* one giant free block */
    head->size    = heap_size - HEADER_SIZE;
    head->next    = head->prev = NULL;
    head->magic   = MAGIC_FREE;
    head->is_free = 1;
    free_head = head;
    FL(head)->prev = FL(head)->next = NULL;
    rover = NULL;

    cur_used = peak_used = 0;
    n_alloc = n_free = n_fail = n_errors = 0;
    corrupted = 0;
    return 0;
}

void allocator_destroy(void)
{
    free(arena);
    arena = NULL; arena_size = 0; head = free_head = rover = NULL;
}

void set_strategy(fit_strategy_t s)   { strategy = s; rover = NULL; }
fit_strategy_t get_strategy(void)     { return strategy; }
void allocator_set_verbose(int on)    { verbose = on; }

const char *strategy_name(fit_strategy_t s)
{
    switch (s) {
        case FIRST_FIT: return "first-fit";
        case NEXT_FIT:  return "next-fit";
        case BEST_FIT:  return "best-fit";
        case WORST_FIT: return "worst-fit";
    }
    return "?";
}

/* ------------------------------ internal helpers -------------------------- */
static inline void note_peak(void) { if (cur_used > peak_used) peak_used = cur_used; }

/* ---- free list: links are stored in the payload of each free block ------- */

/* Take b out of the free list. Keeps the next-fit rover valid. */
static void fl_remove(block_t *b)
{
    flinks_t *l = FL(b);
    if (l->prev) FL(l->prev)->next = l->next; else free_head = l->next;
    if (l->next) FL(l->next)->prev = l->prev;
    if (rover == b) rover = l->next;
}

/* Put free block b into the list at its address-ordered position. The
 * predecessor is the nearest free block physically before b. */
static void fl_insert(block_t *b)
{
    block_t *p = b->prev;
    while (p && !p->is_free) p = p->prev;
    flinks_t *l = FL(b);
    l->prev = p;
    l->next = p ? FL(p)->next : free_head;
    if (p) FL(p)->next = b; else free_head = b;
    if (l->next) FL(l->next)->prev = b;
}

/* Find a free block with payload >= size, using the active strategy.
 * Only the free list is walked, so used blocks cost nothing. */
static block_t *find_block(size_t size)
{
    block_t *b, *best = NULL;

    switch (strategy) {
    case FIRST_FIT:
        for (b = free_head; b; b = FL(b)->next)
            if (b->size >= size) return b;
        return NULL;

    case NEXT_FIT: {
        block_t *start = rover ? rover : free_head;
        if (!start) return NULL;
        b = start;
        do {
            if (b->size >= size) return b;
            b = FL(b)->next ? FL(b)->next : free_head;   /* wrap around */
        } while (b != start);
        return NULL;
    }

    case BEST_FIT:
        for (b = free_head; b; b = FL(b)->next) {
            if (b->size < size) continue;
            if (b->size == size) return b;      /* perfect fit, stop early */
            if (!best || b->size < best->size) best = b;
        }
        return best;

    case WORST_FIT:
        for (b = free_head; b; b = FL(b)->next)
            if (b->size >= size && (!best || b->size > best->size)) best = b;
        return best;
    }
    return NULL;
}

/* Cut b so that its payload is exactly `size`; the rest becomes a new free block
 * (returned, NOT yet linked into the free list) or NULL if no split happened. */
static block_t *split_block(block_t *b, size_t size)
{
    if (b->size < size + MIN_SPLIT) return NULL;   /* leftover too small: keep whole */

    block_t *n = (block_t *)((unsigned char *)b + HEADER_SIZE + size);
    n->size    = b->size - size - HEADER_SIZE;
    n->is_free = 1;
    n->magic   = MAGIC_FREE;
    n->prev    = b;
    n->next    = b->next;
    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = size;
    return n;
}

/* Merge b with the block physically after it. The caller must already have taken
 * that block out of the free list (fl_remove). */
static void merge_with_next(block_t *b)
{
    block_t *n = b->next;
    b->size += HEADER_SIZE + n->size;
    b->next  = n->next;
    if (n->next) n->next->prev = b;
    n->magic = 0;                               /* erase stale header          */
}

/* Validate a user pointer and return its header, or NULL (with a diagnostic). */
static block_t *block_from_ptr(void *p, const char *op)
{
    uintptr_t a = (uintptr_t)p, lo = (uintptr_t)arena;

    if (!arena || corrupted) {
        diag("%s refused: heap not initialised or corrupted", op);
        return NULL;
    }
    if (a < lo + HEADER_SIZE || a >= lo + arena_size || (a % ALIGNMENT)) {
        diag("%s: pointer does not belong to this heap (invalid free)", op);
        n_errors++;
        return NULL;
    }
    block_t *b = (block_t *)(a - HEADER_SIZE);
    if (b->magic == MAGIC_FREE) {
        diag("%s: block already free (DOUBLE FREE detected)", op);
        n_errors++;
        return NULL;
    }
    if (b->magic != MAGIC_USED || b->is_free) {
        diag("%s: bad block header (invalid pointer or corruption)", op);
        n_errors++;
        return NULL;
    }
    return b;
}

/* ------------------------------ public API -------------------------------- */
void *my_malloc(size_t size)
{
    if (size == 0) return NULL;
    if (!arena && allocator_init(DEFAULT_HEAP) != 0) return NULL;
    if (corrupted) { diag("%s refused: heap corrupted, re-init needed", "malloc"); return NULL; }
    if (size > arena_size) { n_fail++; return NULL; }   /* also stops overflow in ALIGN_UP */

    size = ALIGN_UP(size);
    block_t *b = find_block(size);
    if (!b) { n_fail++; return NULL; }

    /* Take b out of the free list; a leftover block (if any) takes its place. */
    block_t *pf = FL(b)->prev, *nf = FL(b)->next;
    block_t *rest = split_block(b, size);
    if (rest) {
        FL(rest)->prev = pf;  FL(rest)->next = nf;
        if (pf) FL(pf)->next = rest; else free_head = rest;
        if (nf) FL(nf)->prev = rest;
        rover = rest;                           /* next-fit resumes right after b */
    } else {
        fl_remove(b);
        rover = nf;
    }
    b->is_free = 0;
    b->magic   = MAGIC_USED;

    cur_used += b->size;
    note_peak();
    n_alloc++;
    return (unsigned char *)b + HEADER_SIZE;
}

void my_free(void *ptr)
{
    if (!ptr) return;                           /* free(NULL) is a no-op, like libc */
    block_t *b = block_from_ptr(ptr, "free");
    if (!b) return;

    cur_used -= b->size;
    n_free++;
    b->is_free = 1;
    b->magic   = MAGIC_FREE;

    /* Coalesce forward, then backward, so adjacent holes become one. */
    if (b->next && b->next->is_free) { fl_remove(b->next); merge_with_next(b); }
    if (b->prev && b->prev->is_free) merge_with_next(b->prev);   /* p absorbs b, p already listed */
    else                             fl_insert(b);
}

void *my_calloc(size_t n, size_t size)
{
    if (n != 0 && size > SIZE_MAX / n) { n_fail++; return NULL; }   /* overflow guard */
    size_t total = n * size;
    void *p = my_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *my_realloc(void *ptr, size_t size)
{
    if (!ptr)       return my_malloc(size);
    if (size == 0)  { my_free(ptr); return NULL; }

    block_t *b = block_from_ptr(ptr, "realloc");
    if (!b) return NULL;
    if (size > arena_size) { n_fail++; return NULL; }

    size_t need = ALIGN_UP(size), old = b->size;

    if (need <= old) {                          /* shrink in place */
        block_t *rest = split_block(b, need);
        if (rest) {                             /* a leftover block was created */
            cur_used -= old - b->size;
            if (rest->next && rest->next->is_free) { fl_remove(rest->next); merge_with_next(rest); }
            fl_insert(rest);
        }
        return ptr;
    }

    /* grow in place if the next block is free and large enough */
    if (b->next && b->next->is_free && old + HEADER_SIZE + b->next->size >= need) {
        fl_remove(b->next);
        merge_with_next(b);
        block_t *rest = split_block(b, need);
        if (rest) fl_insert(rest);
        cur_used += b->size - old;
        note_peak();
        return ptr;
    }

    /* otherwise: allocate new, copy, free old (old block untouched on failure) */
    void *q = my_malloc(size);
    if (!q) return NULL;
    memcpy(q, ptr, old);
    my_free(ptr);
    return q;
}

size_t my_usable_size(void *ptr)
{
    block_t *b = block_from_ptr(ptr, "usable_size");
    return b ? b->size : 0;
}

long allocator_offset(const void *ptr)
{
    return (long)((const unsigned char *)ptr - arena);
}

/* --------------------------- analysis / debugging ------------------------- */
static int block_in_arena(const block_t *b)
{
    uintptr_t a = (uintptr_t)b, lo = (uintptr_t)arena;
    return a >= lo && a + HEADER_SIZE <= lo + arena_size;
}

void get_stats(alloc_stats_t *s)
{
    memset(s, 0, sizeof *s);
    s->heap_size = arena_size;
    s->peak_used = peak_used;
    s->total_allocs = n_alloc;
    s->total_frees = n_free;
    s->failed_allocs = n_fail;
    s->errors_detected = n_errors;

    size_t guard = arena_size / HEADER_SIZE + 1;
    for (block_t *b = head; b && guard-- && block_in_arena(b); b = b->next) {
        s->overhead_bytes += HEADER_SIZE;
        if (b->is_free) {
            s->free_blocks++;
            s->free_bytes += b->size;
            if (b->size > s->largest_free) s->largest_free = b->size;
        } else {
            s->used_blocks++;
            s->used_bytes += b->size;
        }
    }
    s->external_frag = s->free_bytes
        ? 100.0 * (1.0 - (double)s->largest_free / (double)s->free_bytes) : 0.0;
}

void print_stats(void)
{
    alloc_stats_t s; get_stats(&s);
    printf("+----------------- HEAP STATISTICS (%s) -----------------\n", strategy_name(strategy));
    printf("| heap size        : %zu bytes\n", s.heap_size);
    printf("| used (payload)   : %zu bytes in %zu blocks\n", s.used_bytes, s.used_blocks);
    printf("| free (payload)   : %zu bytes in %zu blocks\n", s.free_bytes, s.free_blocks);
    printf("| header overhead  : %zu bytes\n", s.overhead_bytes);
    printf("| largest free blk : %zu bytes\n", s.largest_free);
    printf("| external frag.   : %.1f %%   (0%% = all free space is one piece)\n", s.external_frag);
    printf("| peak usage       : %zu bytes\n", s.peak_used);
    printf("| allocs/frees     : %zu / %zu   failed allocs: %zu   errors caught: %zu\n",
           s.total_allocs, s.total_frees, s.failed_allocs, s.errors_detected);
    printf("+--------------------------------------------------------------\n");
}

void print_heap_map(void)
{
    printf("  #   offset      block size   payload   state\n");
    printf("  --  ----------  -----------  --------  -----\n");
    size_t guard = arena_size / HEADER_SIZE + 1;
    int i = 0;
    for (block_t *b = head; b && guard-- && block_in_arena(b); b = b->next, i++)
        printf("  %-3d 0x%08zx  %-11zu  %-8zu  %s\n", i,
               (size_t)((unsigned char *)b - arena), HEADER_SIZE + b->size, b->size,
               b->is_free ? "FREE" : "USED");
}

void print_heap_bar(int width)
{
    if (!arena) return;
    if (width < 10) width = 10;
    if (width > 200) width = 200;

    double *used = calloc((size_t)width, sizeof *used);
    if (!used) return;
    double cell = (double)arena_size / width;

    size_t guard = arena_size / HEADER_SIZE + 1;
    for (block_t *b = head; b && guard-- && block_in_arena(b); b = b->next) {
        if (b->is_free) continue;
        double off = (double)((unsigned char *)b - arena);
        double end = off + (double)(HEADER_SIZE + b->size);
        if (end > (double)arena_size) end = (double)arena_size;
        for (int c = (int)(off / cell); c < width && c * cell < end; c++) {
            double lo = off > c * cell ? off : c * cell;
            double hi = end < (c + 1) * cell ? end : (c + 1) * cell;
            if (hi > lo) used[c] += (hi - lo) / cell;
        }
    }

    int color = isatty(STDOUT_FILENO);
    printf("  [");
    for (int c = 0; c < width; c++) {
        const char *on = "", *off = "";
        char ch;
        if (used[c] > 0.85)      { ch = '#'; on = color ? "\x1b[31m" : ""; }
        else if (used[c] < 0.15) { ch = '.'; on = color ? "\x1b[32m" : ""; }
        else                     { ch = '+'; on = color ? "\x1b[33m" : ""; }
        if (color) off = "\x1b[0m";
        printf("%s%c%s", on, ch, off);
    }
    printf("]  # used  . free  + mixed\n");
    free(used);
}

void print_leaks(void)
{
    size_t count = 0, bytes = 0;
    size_t guard = arena_size / HEADER_SIZE + 1;
    for (block_t *b = head; b && guard-- && block_in_arena(b); b = b->next)
        if (!b->is_free) {
            printf("  LEAK: %zu bytes at heap offset 0x%zx\n", b->size,
                   (size_t)((unsigned char *)b - arena) + HEADER_SIZE);
            count++; bytes += b->size;
        }
    if (count) printf("  => %zu leaked block(s), %zu bytes in total\n", count, bytes);
    else       printf("  No leaks: every allocated block was freed.\n");
}

/*
 * check_heap: verify every invariant of the block list.
 *   - links (prev/next) are consistent and inside the arena
 *   - magic number matches the free/used flag
 *   - header + payload exactly touches the next header (no gaps/overlaps)
 *   - no two adjacent free blocks (coalescing invariant)
 */
int check_heap(void)
{
    if (!arena) return 0;
    int errs = 0;
    block_t *prev = NULL;
    size_t guard = arena_size / HEADER_SIZE + 1;
    unsigned char *end = arena + arena_size;

    for (block_t *b = head; b; prev = b, b = b->next) {
        if (!guard-- || !block_in_arena(b)) { diag("%s", "block list leaves the arena / loops"); errs++; break; }
        if (b->prev != prev)               { diag("%s", "prev pointer mismatch");              errs++; }
        if (b->magic != (b->is_free ? MAGIC_FREE : MAGIC_USED)) { diag("%s", "magic number wrong"); errs++; }
        if (b->size > arena_size)          { diag("%s", "block size larger than heap");        errs++; break; }

        unsigned char *expect = (unsigned char *)b + HEADER_SIZE + b->size;
        if (b->next ? (unsigned char *)b->next != expect : expect != end) {
            diag("%s", "block size does not match position of next block (overflow?)");
            errs++;
        }
        if (prev && prev->is_free && b->is_free) { diag("%s", "two adjacent free blocks (missed coalesce)"); errs++; }
    }

    /* Free list: only walked if the block list is sane (else we'd follow garbage). */
    if (!errs) {
        size_t listed = 0, physical = 0;
        block_t *last = NULL;
        guard = arena_size / HEADER_SIZE + 1;
        for (block_t *b = head; b; b = b->next) if (b->is_free) physical++;
        for (block_t *b = free_head; b; last = b, b = FL(b)->next) {
            if (!guard-- || !block_in_arena(b) || !b->is_free || b->magic != MAGIC_FREE) {
                diag("%s", "free list contains a bad block"); errs++; break;
            }
            if (FL(b)->prev != last)           { diag("%s", "free list prev link mismatch"); errs++; }
            if (last && last >= b)             { diag("%s", "free list not in address order"); errs++; }
            listed++;
        }
        if (!errs && listed != physical)       { diag("%s", "free list misses a free block"); errs++; }
    }
    if (errs) { corrupted = 1; n_errors += (size_t)errs; }
    return errs;
}

int allocator_inject_overflow(void *ptr)
{
    block_t *b = block_from_ptr(ptr, "inject");
    if (!b || !b->next) return -1;
    memset(b->next, 'A', sizeof(size_t));       /* overwrite next header's size field */
    return 0;
}

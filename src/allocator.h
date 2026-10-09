/*
 * allocator.h - Public API of the custom memory allocator (malloc/free clone)
 *
 * Project : Simple Memory Allocator   (Hackathon Abhinava, REVA University)
 * Design  : explicit doubly linked list of ALL blocks (used + free),
 *           kept in address order, with block splitting and coalescing.
 */
#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

/* Placement strategies: how we choose a free block for a request. */
typedef enum {
    FIRST_FIT = 0,   /* first free block that is big enough            */
    NEXT_FIT,        /* like first-fit, but resume from last position  */
    BEST_FIT,        /* smallest free block that is big enough         */
    WORST_FIT        /* largest free block (leaves biggest leftover)   */
} fit_strategy_t;

/* Snapshot of the heap, produced by get_stats(). */
typedef struct {
    size_t heap_size;        /* total arena size in bytes                     */
    size_t used_bytes;       /* payload bytes inside allocated blocks         */
    size_t free_bytes;       /* payload bytes inside free blocks              */
    size_t overhead_bytes;   /* bytes consumed by block headers               */
    size_t used_blocks;
    size_t free_blocks;
    size_t largest_free;     /* biggest single request that can still succeed */
    size_t peak_used;        /* high-water mark of used_bytes                 */
    size_t total_allocs;     /* successful allocations                        */
    size_t total_frees;      /* successful frees                              */
    size_t failed_allocs;    /* requests that returned NULL                   */
    size_t errors_detected;  /* double free / invalid free / corruption       */
    double external_frag;    /* % : 100 * (1 - largest_free / free_bytes)     */
} alloc_stats_t;

/* ---- lifecycle -------------------------------------------------------- */
int         allocator_init(size_t heap_size);   /* 0 on success, -1 on error */
void        allocator_destroy(void);
void        set_strategy(fit_strategy_t s);
fit_strategy_t get_strategy(void);
const char *strategy_name(fit_strategy_t s);
void        allocator_set_verbose(int on);      /* print diagnostics to stderr */

/* ---- the allocator itself -------------------------------------------- */
void *my_malloc(size_t size);
void  my_free(void *ptr);
void *my_calloc(size_t n, size_t size);
void *my_realloc(void *ptr, size_t size);
size_t my_usable_size(void *ptr);               /* 0 if ptr is invalid */

/* ---- analysis / debugging tools --------------------------------------- */
void  get_stats(alloc_stats_t *out);
void  print_stats(void);
void  print_heap_map(void);                     /* table of every block        */
void  print_heap_bar(int width);                /* one-line visual of the heap */
void  print_leaks(void);                        /* blocks that were never freed*/
int   check_heap(void);                         /* 0 = healthy, else #errors   */
long  allocator_offset(const void *ptr);        /* byte offset inside the heap */

/* Fault injection for the demo: simulates a buffer overflow that tramples the
 * header of the next block. check_heap() must then report corruption.        */
int   allocator_inject_overflow(void *ptr);

#endif /* ALLOCATOR_H */

/*
 * demo.c - Interactive "heap playground" for the live demo.
 *
 *   ./bin/demo               -> interactive shell (type 'help')
 *   ./bin/demo --showdown    -> automatic first/next/best/worst-fit comparison
 *   ./bin/demo < script.txt  -> run commands from a file (no prompt)
 */
#include "allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_IDS   512
#define BAR_WIDTH 64
#define HEAP_SIZE 4096

static void *table[MAX_IDS];
static int   next_id = 1;

static void show(void) { print_heap_bar(BAR_WIDTH); }

static void help(void)
{
    printf(
"  alloc <bytes>            allocate memory, prints an id        (alias: malloc)\n"
"  free <id>                free a block\n"
"  realloc <id> <bytes>     resize a block\n"
"  strategy first|next|best|worst   change placement policy\n"
"  map                      table of every block (offset / size / state)\n"
"  bar                      one-line picture of the heap\n"
"  stats                    usage, fragmentation, error counters\n"
"  check                    verify heap integrity\n"
"  leaks                    list blocks that are still allocated\n"
"  overflow <id>            SIMULATE a buffer overflow into the next block\n"
"  tamper-free <id>         free the same id twice (shows double-free detection)\n"
"  reset [bytes]            fresh heap (default %d bytes)\n"
"  showdown                 compare all 4 strategies on the same scenario\n"
"  help | quit\n", HEAP_SIZE);
}

static void reset_heap(size_t size)
{
    allocator_init(size);
    memset(table, 0, sizeof table);
    next_id = 1;
}

/* Same layout as the unit test: four holes of 112/304/160/512 bytes, request 140. */
static void showdown(void)
{
    const fit_strategy_t S[] = { FIRST_FIT, NEXT_FIT, BEST_FIT, WORST_FIT };
    size_t sz[4] = { 100, 300, 150, 500 };
    printf("\nSTRATEGY SHOWDOWN: four free holes (112, 304, 160, 512 bytes) separated by\n"
           "small used blocks. Where does a 140-byte request land?\n");

    for (int s = 0; s < 4; s++) {
        void *hole[4];
        allocator_init(HEAP_SIZE);
        set_strategy(S[s]);
        for (int i = 0; i < 4; i++) { hole[i] = my_malloc(sz[i]); my_malloc(16); }
        alloc_stats_t st; get_stats(&st);
        my_malloc(st.largest_free);
        for (int i = 0; i < 4; i++) my_free(hole[i]);

        printf("\n--- %s ---\n  before:", strategy_name(S[s]));
        show();
        void *p = my_malloc(140);
        int which = -1;
        for (int i = 0; i < 4; i++) if (p == hole[i]) which = i;
        printf("  after :");
        show();
        printf("  -> placed at heap offset %ld (%s)\n", allocator_offset(p),
               which == 1 ? "hole #2, 304 B: first one big enough" :
               which == 2 ? "hole #3, 160 B: tightest fit, least waste" :
               which == 3 ? "hole #4, 512 B: biggest hole" : "elsewhere");
    }
    printf("\nTake-away: best-fit wastes least now but leaves tiny useless slivers;\n"
           "worst-fit keeps big leftovers but destroys the big holes. See 'make bench'.\n\n");
    reset_heap(HEAP_SIZE);
    set_strategy(FIRST_FIT);
}

static void *get(int id)
{
    if (id < 1 || id >= MAX_IDS || !table[id]) { printf("  no such live id: %d\n", id); return NULL; }
    return table[id];
}

int main(int argc, char **argv)
{
    int tty = isatty(STDIN_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);          /* keep stdout/stderr in order */
    reset_heap(HEAP_SIZE);

    if (argc > 1 && strcmp(argv[1], "--showdown") == 0) { showdown(); return 0; }

    printf("=== Custom Memory Allocator - interactive demo (heap %d bytes) ===\n", HEAP_SIZE);
    printf("Type 'help' for commands. Start with:  alloc 100   alloc 300   free 1\n\n");
    show();

    char line[256];
    for (;;) {
        if (tty) { printf("heap[%s]> ", strategy_name(get_strategy())); fflush(stdout); }
        if (!fgets(line, sizeof line, stdin)) break;
        if (!tty) printf(">> %s", line);        /* echo when reading from a script */

        char *cmd = strtok(line, " \t\r\n");
        if (!cmd) continue;
        char *a1 = strtok(NULL, " \t\r\n"), *a2 = strtok(NULL, " \t\r\n");

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) break;
        else if (!strcmp(cmd, "help")) help();
        else if (!strcmp(cmd, "alloc") || !strcmp(cmd, "malloc")) {
            if (!a1) { printf("  usage: alloc <bytes>\n"); continue; }
            size_t n = strtoul(a1, NULL, 10);
            void *p = my_malloc(n);
            if (!p) printf("  allocation of %zu bytes FAILED (out of memory, too fragmented, or heap corrupted)\n", n);
            else if (next_id >= MAX_IDS) { printf("  id table full\n"); my_free(p); }
            else {
                table[next_id] = p;
                printf("  id %d -> offset %ld, usable %zu bytes\n", next_id, allocator_offset(p), my_usable_size(p));
                next_id++;
            }
            show();
        } else if (!strcmp(cmd, "free")) {
            int id = a1 ? atoi(a1) : 0; void *p = get(id);
            if (p) { my_free(p); table[id] = NULL; printf("  freed id %d\n", id); show(); }
        } else if (!strcmp(cmd, "tamper-free")) {
            int id = a1 ? atoi(a1) : 0; void *p = get(id);
            if (p) { my_free(p); my_free(p); table[id] = NULL; show(); }
        } else if (!strcmp(cmd, "realloc")) {
            int id = a1 ? atoi(a1) : 0; void *p = get(id);
            if (p && a2) {
                void *q = my_realloc(p, strtoul(a2, NULL, 10));
                if (q) { table[id] = q; printf("  id %d now at offset %ld (%s)\n", id, allocator_offset(q), q == p ? "resized in place" : "moved"); }
                else printf("  realloc failed, original block unchanged\n");
                show();
            } else if (!a2) printf("  usage: realloc <id> <bytes>\n");
        } else if (!strcmp(cmd, "strategy")) {
            if (!a1) printf("  usage: strategy first|next|best|worst\n");
            else if (!strcmp(a1, "first")) set_strategy(FIRST_FIT);
            else if (!strcmp(a1, "next"))  set_strategy(NEXT_FIT);
            else if (!strcmp(a1, "best"))  set_strategy(BEST_FIT);
            else if (!strcmp(a1, "worst")) set_strategy(WORST_FIT);
            else printf("  unknown strategy\n");
            printf("  strategy = %s\n", strategy_name(get_strategy()));
        } else if (!strcmp(cmd, "map"))    print_heap_map();
        else if (!strcmp(cmd, "bar"))      show();
        else if (!strcmp(cmd, "stats"))    print_stats();
        else if (!strcmp(cmd, "leaks"))    print_leaks();
        else if (!strcmp(cmd, "check")) {
            int e = check_heap();
            if (e == 0) printf("  heap OK: all invariants hold\n");
            else        printf("  HEAP CORRUPTED: %d problem(s) found. Type 'reset' to start over.\n", e);
        } else if (!strcmp(cmd, "overflow")) {
            int id = a1 ? atoi(a1) : 0; void *p = get(id);
            if (p) {
                if (allocator_inject_overflow(p) == 0) printf("  wrote past the end of id %d (smashed next block's header). Now type 'check'.\n", id);
                else printf("  id %d is the last block: nothing after it to corrupt\n", id);
            }
        } else if (!strcmp(cmd, "reset")) {
            reset_heap(a1 ? strtoul(a1, NULL, 10) : HEAP_SIZE);
            printf("  new heap created\n"); show();
        } else if (!strcmp(cmd, "showdown")) showdown();
        else printf("  unknown command '%s' (try 'help')\n", cmd);
    }
    printf("\nOn exit:\n"); print_leaks();
    allocator_destroy();
    return 0;
}

# Simple Memory Allocator (`malloc` / `free` clone) in C

**Hackathon Abhinava — REVA University, School of CSE | B25CS0311 Portfolio Building | Problem Statement 12**

A custom heap allocator written from scratch in C: `my_malloc`, `my_free`, `my_calloc`, and `my_realloc`, with four placement strategies (first-fit, next-fit, best-fit, worst-fit), block splitting and coalescing, corruption/double-free detection, leak reporting, an interactive heap visualizer, and benchmarking against glibc `malloc`.

## Features

- **Core API:** `my_malloc`, `my_free`, `my_calloc`, `my_realloc`, and `my_usable_size`.
- **Placement strategies:** `FIRST_FIT`, `NEXT_FIT`, `BEST_FIT`, and `WORST_FIT`.
- **Fragmentation control:** block splitting, immediate coalescing, and 16-byte alignment.
- **Safety checks:** double-free, invalid-pointer, and heap-corruption detection using header magic numbers.
- **Diagnostics:** heap statistics, heap map/bar, leak report, and `check_heap()`.
- **Tests and comparison:** correctness/stress tests plus benchmark workloads versus glibc `malloc`.

## Build and run (Linux / WSL / macOS with GCC and Make)

```bash
make            # build test suite, benchmark, and interactive demo
make test       # run correctness and stress tests
make demo       # interactive heap playground
make showdown   # compare placement strategies on the same scenario
make bench      # benchmark workloads and write results.csv
make plot       # generate comparison.png (requires Python + matplotlib)
make sanitize   # run tests with AddressSanitizer and UBSan
make valgrind   # optional leak check (requires Valgrind)
```

On Windows, use WSL and install the build tools with `sudo apt install build-essential`.

Run a scripted demonstration with `./bin/demo < samples/demo_script.txt`.

## Example

```c
#include "src/allocator.h"

allocator_init(1 << 20);          // 1 MiB heap
set_strategy(BEST_FIT);
int *a = my_malloc(10 * sizeof *a);
my_free(a);
print_stats();
```

## Project layout

```text
src/allocator.h       public API
src/allocator.c       allocator implementation
src/demo.c            interactive heap playground
 tests/test_allocator.c correctness and stress tests
bench/benchmark.c     benchmark versus libc malloc
bench/plot_results.py chart generator
docs/                 report, demo script, and team sheet
samples/              sample outputs, benchmark CSV, and demo script
```

See `docs/REPORT.md` for the benchmark discussion and `docs/DEMO_SCRIPT.md` for a short live demo plan.

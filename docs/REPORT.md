# Comparison Report: Custom Allocator vs glibc `malloc`

Machine: Linux x86-64, GCC 13 `-O2`. Each figure is the best of 3 runs. Every allocator receives the **identical,
deterministic request stream** (seeded xorshift RNG), so the comparison is fair. Raw data: `samples/results.csv`.

## 1. Workloads
| Name | What it does | Why |
|---|---|---|
| sequential | 20,000 x 64 B allocated, then freed in order | best case, no fragmentation |
| churn | 100,000 random alloc/free ops, 2,000 slots, 16-1024 B | typical long-running program |
| phased | 40 waves: 400 small blocks, free 55%, 60 large blocks, free 35% | **designed to fragment** the heap |

## 2. Results (final design)
| Workload | Allocator | Time (ms) | Free blocks | External frag. | Largest free |
|---|---|---:|---:|---:|---:|
| sequential | first-fit | 0.48 | 1 | 0.0 % | 2220 KB |
| sequential | libc malloc | 0.75 | - | - | - |
| churn | first-fit | 26.3 | 293 | 1.4 % | 7561 KB |
| churn | next-fit | 5.0 | 481 | 71.0 % | 2220 KB |
| churn | best-fit | 37.4 | 147 | **1.1 %** | 7589 KB |
| churn | worst-fit | 197.3 | 494 | 92.5 % | 572 KB |
| churn | libc malloc | 4.8 | - | - | - |
| phased | first-fit | 3.1 | 138 | 10.5 % | 1675 KB |
| phased | next-fit | 5.1 | 142 | 57.9 % | 788 KB |
| phased | best-fit | 5.2 | 130 | **10.3 %** | 1679 KB |
| phased | worst-fit | 12.9 | 139 | 89.7 % | 193 KB |
| phased | libc malloc | 1.6 | - | - | - |

External fragmentation = `100 x (1 - largest_free_block / total_free_bytes)`. 0 % means all free memory is one piece.
Times fluctuate by a few % between runs; the ranking does not. Re-run with `make bench`. See `samples/comparison.png`.

## 3. Findings
1. **First-fit and best-fit fragment least** (about 1 % on churn, about 10 % on the hostile workload). Best-fit is marginally
   better on fragmentation, first-fit is faster because it stops at the first match.
2. **Next-fit is fast but fragments badly** (58-71 %): it keeps allocating after the last position and never reuses
   early holes, so small leftovers pile up across the whole heap.
3. **Worst-fit is the loser on both axes**: it must scan every free block, and carving from the largest hole
   destroys the big blocks (largest free block shrinks from about 7.5 MB to 572 KB).
4. **glibc is still faster** when the heap is fragmented (about 5-8x on churn) thanks to size-class bins and thread caches. Our
   first-fit cost grows with the number of free blocks; that is the price of a simple linked-list design.

## 4. Optimisation we made during the hackathon (before / after)
The first version kept one list of *all* blocks, so `malloc` had to step over every used block. Adding a second,
address-ordered **free list stored inside the free payloads** (no header growth) fixed this:

| Workload (time in ms) | Before | After | Speed-up |
|---|---:|---:|---:|
| sequential, first-fit | 958.7 | 0.48 | about 2000x |
| sequential, best-fit | 950.4 | 0.40 | about 2400x |
| churn, first-fit | 198.7 | 26.3 | about 7.5x |
| churn, best-fit | 286.9 | 37.4 | about 7.7x |
| churn, worst-fit | 537.4 | 197.3 | about 2.7x |
| phased, first-fit | 22.6 | 3.1 | about 7x |

Fragmentation results for first-fit and best-fit were **identical** before and after: behaviour preserved, only speed changed.
(Next-fit's rover now points into the free list, so its numbers shift slightly.)

## 5. Correctness evidence
* `make test`: 52 checks pass (alignment, no overlap, split, reuse, coalescing in 3 free orders, error detection,
  OOM, calloc overflow, realloc grow/shrink/in-place, strategy behaviour on a crafted heap).
* 30,000 random malloc/realloc/free ops per strategy; every block filled with a pattern and verified before free.
* `check_heap()` verifies links, magic numbers, block geometry, no adjacent free blocks and free-list consistency.
* `make sanitize`: same tests under AddressSanitizer + UBSan - no errors.

## 6. What we would do differently with more time
* Segregated free lists (size classes) for near O(1) allocation, as glibc and jemalloc do.
* Boundary tags (footers) so backward coalescing and `fl_insert` never walk the block list.
* Grow the arena with `mmap`/`sbrk`; large requests via direct `mmap`.
* Thread safety (mutex or per-thread arenas); canary bytes to detect overflows at `free` time.

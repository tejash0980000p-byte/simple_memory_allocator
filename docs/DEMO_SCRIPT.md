# 3-5 Minute Live Demo Script

Before presenting: `make && make test` once so nothing needs compiling live. Terminal font large. Keep `samples/` outputs as backup.

| Time | Who | What to do / say |
|---|---|---|
| 0:00 | Presenter 1 | "We built malloc/free from scratch in C." Show `src/allocator.h` (the API) and the picture of `[header \| payload]` blocks. |
| 0:30 | Presenter 2 | `make test` -> **52 passed, 0 failed**. Mention: runs for all 4 strategies and under AddressSanitizer. |
| 1:00 | Presenter 2 | `make demo`, then type: `alloc 100`, `alloc 300`, `alloc 150`, `alloc 500`, `free 2`, `free 4`, `bar`, `stats`. Point at the bar: `#` used, `.` free. Explain splitting + coalescing. |
| 2:00 | Presenter 3 | `make showdown`. Same heap, same 140-byte request -> first-fit picks the 304 B hole, best-fit the 160 B hole, worst-fit the 512 B hole. |
| 2:45 | Presenter 3 | **Judge interaction:** type `alloc 100`, `alloc 100`, `overflow 1`, `check` -> heap flagged **CORRUPTED**; then `reset`, `alloc 64`, `tamper-free 1` -> **DOUBLE FREE detected**. (`showdown` resets the heap, so allocate fresh blocks first.) |
| 3:30 | Presenter 4 | `make bench` (or show `samples/comparison.png`). Say: first/best-fit about 1-10 % fragmentation, worst-fit about 90 %; glibc faster on fragmented heaps because of size-class bins. |
| 4:30 | Presenter 4 | Future scope (segregated lists, boundary tags, mmap, thread safety). |

Backup if the live demo fails: `./bin/demo < samples/demo_script.txt` or the screen recording.

## Questions judges are likely to ask
* **How does coalescing work?** On `free`, check the physical next and previous block; if free, merge headers and sizes into one block.
* **Why is yours slower than glibc?** glibc has size-class bins and thread caches; ours is a linked list so cost grows with free blocks. We already sped it up 7-2000x by adding a free list.
* **Why 16-byte alignment?** x86-64 ABI: `long double` and SSE types need 16-byte aligned memory. Header is 32 B so the payload stays aligned.
* **Which strategy is best?** Best-fit for least fragmentation, first-fit for the best speed/fragmentation balance. Worst-fit and next-fit lose in our measurements.
* **How do you detect a double free?** The header holds a magic number: `0xF4EEB10C` when free, `0xA110C8ED` when used. Freeing a block whose magic is "free" is flagged.
* **Where is the free list stored?** Inside the payload of each free block (a block's data is unused while free), so no extra header memory.
* **Is it thread safe?** No, single-threaded by design; adding a mutex around malloc/free is the first step.

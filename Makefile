# Simple Memory Allocator - build file          (Linux / WSL / macOS, needs gcc + make)
CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2
SRC     = src/allocator.c
BIN    ?= bin

all: $(BIN)/test_allocator $(BIN)/benchmark $(BIN)/demo

$(BIN):
	mkdir -p $(BIN)

$(BIN)/test_allocator: tests/test_allocator.c $(SRC) src/allocator.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ tests/test_allocator.c $(SRC)

$(BIN)/benchmark: bench/benchmark.c $(SRC) src/allocator.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ bench/benchmark.c $(SRC)

$(BIN)/demo: src/demo.c $(SRC) src/allocator.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/demo.c $(SRC)

test: $(BIN)/test_allocator          ## run the correctness test-suite
	./$(BIN)/test_allocator

bench: $(BIN)/benchmark              ## compare strategies + glibc malloc (writes results.csv)
	./$(BIN)/benchmark

demo: $(BIN)/demo                    ## interactive heap playground
	./$(BIN)/demo

showdown: $(BIN)/demo                ## automatic first/next/best/worst-fit comparison
	./$(BIN)/demo --showdown

plot: bench                       ## optional: draw charts from results.csv (needs matplotlib)
	python3 bench/plot_results.py

# Extra safety net: compile tests with AddressSanitizer + UBSan
sanitize: tests/test_allocator.c $(SRC) | $(BIN)
	$(CC) -std=gnu11 -g -fsanitize=address,undefined -o $(BIN)/test_asan tests/test_allocator.c $(SRC)
	./$(BIN)/test_asan

# Optional memory-leak check of the allocator itself (needs valgrind)
valgrind: $(BIN)/test_allocator
	valgrind --leak-check=full --error-exitcode=1 ./$(BIN)/test_allocator

clean:
	rm -rf bin results.csv *.png

.PHONY: all test bench demo showdown plot sanitize valgrind clean

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: numa_mmap_bench fault_cost

numa_mmap_bench: numa_mmap_bench.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

fault_cost: fault_cost.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f numa_mmap_bench fault_cost

.PHONY: all clean

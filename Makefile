CC     = g++
CFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native

cacheon: cacheon.cpp sim.cpp sim.h cli.h
	$(CC) $(CFLAGS) cacheon.cpp sim.cpp -o cacheon

# Debug build: enables CACHEON_DEBUG trace output and disables optimisations
debug: cacheon.cpp sim.cpp sim.h cli.h
	$(CC) -std=c++17 -Wall -Wextra -O0 -g -DCACHEON_DEBUG cacheon.cpp sim.cpp -o cacheon_debug

# Example override build: custom cycle costs and default stride
#   make override CACHEON_CYCLES_L1=5 CACHEON_CYCLES_L2=15 CACHEON_DEFAULT_STRIDE=128
override: cacheon.cpp sim.cpp sim.h cli.h
	$(CC) $(CFLAGS) \
	  $(if $(CACHEON_CYCLES_L1),    -DCACHEON_CYCLES_L1=$(CACHEON_CYCLES_L1))       \
	  $(if $(CACHEON_CYCLES_L2),    -DCACHEON_CYCLES_L2=$(CACHEON_CYCLES_L2))       \
	  $(if $(CACHEON_CYCLES_L3),    -DCACHEON_CYCLES_L3=$(CACHEON_CYCLES_L3))       \
	  $(if $(CACHEON_CYCLES_MEMORY),-DCACHEON_CYCLES_MEMORY=$(CACHEON_CYCLES_MEMORY)) \
	  $(if $(CACHEON_DEFAULT_STRIDE),-DCACHEON_DEFAULT_STRIDE=$(CACHEON_DEFAULT_STRIDE)) \
	  cacheon.cpp sim.cpp -o cacheon

run: cacheon
	for size in 4K 8K 16K 32K 64K 128K 256K 512K 1M 2M 4M 8M 16M 32M 64M 128M 256M; do \
		echo "$$size (sequential):"; \
		./cacheon $$size 64; \
		echo "$$size (random):"; \
		./cacheon $$size 64 -r; \
		echo ""; \
	done

clean:
	rm -f cacheon cacheon.exe cacheon_debug cacheon_debug.exe

.PHONY: run debug override clean

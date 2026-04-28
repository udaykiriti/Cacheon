CC = g++
CFLAGS = -std=c++17 -Wall -O2

cacheon: cacheon.cpp sim.cpp
	$(CC) $(CFLAGS) cacheon.cpp sim.cpp -o cacheon

run: cacheon
	for size in 4K 8K 16K 32K 64K 128K 256K 512K 1M 2M 4M 8M 16M 32M 64M 128M 256M; do \
		echo "$$size (sequential):"; \
		./cacheon $$size 64; \
		echo "$$size (random):"; \
		./cacheon $$size 64 -r; \
		echo ""; \
	done

clean:
	rm -f cacheon

.PHONY: run clean

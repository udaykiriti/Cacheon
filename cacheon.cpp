#include <iostream>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <random>
#include <iomanip>
#include <deque>

// Cycle times for AMAT calculation
const uint64_t CYCLES_L1 = 4;
const uint64_t CYCLES_L2 = 12;
const uint64_t CYCLES_L3 = 40;
const uint64_t CYCLES_MEMORY = 200;

// Cache configuration
struct Cache {
    uint64_t size;
    uint64_t lineSize;
    uint64_t associativity;
    uint64_t hits = 0;
    uint64_t misses = 0;
    bool useLRU;
    
    std::vector<std::unordered_set<uint64_t>> sets;
    std::vector<std::deque<uint64_t>> setOrder;  // For LRU tracking

    Cache(uint64_t sz, uint64_t line, uint64_t assoc, bool lru = false)
        : size(sz), lineSize(line), associativity(assoc), useLRU(lru) {
        uint64_t numSets = size / (lineSize * associativity);
        sets.resize(numSets);
        if (useLRU) setOrder.resize(numSets);
    }

    bool access(uint64_t addr) {
        uint64_t numSets = sets.size();
        uint64_t setIndex = (addr / lineSize) % numSets;
        uint64_t tag = addr / (lineSize * numSets);

        if (sets[setIndex].count(tag)) {
            hits++;
            if (useLRU) {
                auto& order = setOrder[setIndex];
                order.erase(std::find(order.begin(), order.end(), tag));
                order.push_back(tag);
            }
            return true;
        }

        misses++;
        if (sets[setIndex].size() < associativity) {
            sets[setIndex].insert(tag);
            if (useLRU) setOrder[setIndex].push_back(tag);
        } else {
            if (useLRU) {
                uint64_t victim = setOrder[setIndex].front();
                setOrder[setIndex].pop_front();
                sets[setIndex].erase(victim);
            } else {
                sets[setIndex].erase(*sets[setIndex].begin());
            }
            sets[setIndex].insert(tag);
            if (useLRU) setOrder[setIndex].push_back(tag);
        }
        return false;
    }

    double hitRate() const {
        uint64_t total = hits + misses;
        return total ? (100.0 * hits / total) : 0.0;
    }

    void reset() {
        hits = 0;
        misses = 0;
        for (auto& s : sets) s.clear();
        for (auto& o : setOrder) o.clear();
    }
};

std::string makeBar(double percentage, int width = 20) {
    int filled = (int)(percentage / 100.0 * width);
    std::string bar = "[";
    for (int i = 0; i < width; i++) {
        bar += (i < filled) ? "=" : " ";
    }
    bar += "]";
    return bar;
}

// Parse size strings like "8M", "256K", "4k" with validation
uint64_t parseSize(const char* str) {
    if (!str || *str == '\0') {
        std::cerr << "Error: Empty size string\n";
        exit(1);
    }
    
    char* endptr;
    uint64_t num = strtoll(str, &endptr, 10);
    
    if (num == 0 || (endptr == str && *str != '0')) {
        std::cerr << "Error: Invalid size '" << str << "'\n";
        exit(1);
    }
    
    if (!endptr || *endptr == '\0') return num;
    
    switch (tolower(*endptr)) {
        case 'k': return num * 1024;
        case 'm': return num * 1024 * 1024;
        case 'g': return num * 1024 * 1024 * 1024;
        default:
            std::cerr << "Error: Unknown suffix '" << *endptr << "' in size\n";
            exit(1);
    }
}

void printResults(const Cache& l1d, const Cache& l2, const Cache& l3, 
                  uint64_t totalAccesses, bool sequential) {
    double l1dRate = l1d.hitRate();
    double l2Rate = l2.hitRate();
    double l3Rate = l3.hitRate();
    
    // Calculate AMAT (Average Memory Access Time in cycles)
    double amat = CYCLES_L1 * (l1dRate / 100.0) +
                  CYCLES_L2 * ((100.0 - l1dRate) / 100.0) * (l2Rate / 100.0) +
                  CYCLES_L3 * ((100.0 - l1dRate) / 100.0) * ((100.0 - l2Rate) / 100.0) * (l3Rate / 100.0) +
                  CYCLES_MEMORY * ((100.0 - l1dRate) / 100.0) * ((100.0 - l2Rate) / 100.0) * ((100.0 - l3Rate) / 100.0);

    std::cout << "\nCACHE REPORT\n";
    std::cout << "============\n\n";
    std::cout << "Total Accesses: " << totalAccesses << "\n";
    std::cout << "Pattern: " << (sequential ? "Sequential" : "Random") << "\n\n";
    
    std::cout << "L1D Cache (32KB):\n";
    std::cout << "  " << makeBar(l1dRate) << " " << std::fixed << std::setprecision(2) << l1dRate << "%\n";
    std::cout << "  Hits: " << l1d.hits << " | Misses: " << l1d.misses << "\n\n";
    
    std::cout << "L2 Cache (256KB):\n";
    std::cout << "  " << makeBar(l2Rate) << " " << std::fixed << std::setprecision(2) << l2Rate << "%\n";
    std::cout << "  Hits: " << l2.hits << " | Misses: " << l2.misses << "\n\n";
    
    std::cout << "L3 Cache (8MB):\n";
    std::cout << "  " << makeBar(l3Rate) << " " << std::fixed << std::setprecision(2) << l3Rate << "%\n";
    std::cout << "  Hits: " << l3.hits << " | Misses: " << l3.misses << "\n\n";
    
    std::cout << "AMAT (Average Memory Access Time): " << std::fixed << std::setprecision(2) << amat << " cycles\n";
}

int main(int argc, char* argv[]) {
    uint64_t testSize = 8 * 1024 * 1024;  // Default 8MB
    uint64_t stride = 64;                  // Default 64 bytes
    bool randomAccess = false;
    bool quiet = false;
    bool runAllSizes = false;
    bool useLRU = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: cacheon [SIZE] [STRIDE] [OPTIONS]\n\n";
            std::cout << "SIZE     Memory size to test (default: 8M)\n";
            std::cout << "         Examples: 4K, 8M, 256M, 1G\n\n";
            std::cout << "STRIDE   Stride in bytes (default: 64)\n\n";
            std::cout << "OPTIONS:\n";
            std::cout << "  -H, --hugepage    Use hugepage-like mode\n";
            std::cout << "  -r, --random      Random access pattern\n";
            std::cout << "  -Hr, -rH          Both hugepage and random\n";
            std::cout << "  -q, --quiet       Quiet mode (no output)\n";
            std::cout << "  -l, --lru         Use LRU replacement (default: FIFO)\n";
            std::cout << "  --all-sizes       Run full benchmark (4K to 256M)\n";
            std::cout << "  -h, --help        Show this help\n";
            std::cout << "  -v, --version     Show version\n\n";
            std::cout << "Examples:\n";
            std::cout << "  cacheon 8M 64\n";
            std::cout << "  cacheon 128M 64 -r\n";
            std::cout << "  cacheon 256M 256 -Hr\n";
            std::cout << "  cacheon 4M 64 -l\n";
            std::cout << "  cacheon --all-sizes\n";
            return 0;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            std::cout << "Cacheon 1.1\n";
            return 0;
        }
        else if (strcmp(argv[i], "--all-sizes") == 0) {
            runAllSizes = true;
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        }
        else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--random") == 0) {
            randomAccess = true;
        }
        else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--lru") == 0) {
            useLRU = true;
        }
        else if (strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--hugepage") == 0) {
            // Hugepage flag accepted but not implemented (advisory only)
        }
        else if (strcmp(argv[i], "-Hr") == 0 || strcmp(argv[i], "-rH") == 0) {
            randomAccess = true;
            // Hugepage flag accepted but not implemented
        }
        else if (argv[i][0] != '-') {
            // Positional arguments: first is size, second is stride
            if (i == 1) {
                testSize = parseSize(argv[i]);
            } else if (i == 2) {
                stride = parseSize(argv[i]);
            }
        }
    }
    
    // Input validation
    if (stride == 0) {
        std::cerr << "Error: Stride cannot be zero\n";
        return 1;
    }
    if (stride > testSize) {
        std::cerr << "Error: Stride (" << stride << ") cannot exceed test size (" << testSize << ")\n";
        return 1;
    }

    // Run full benchmark if requested
    if (runAllSizes) {
        uint64_t sizes[] = {4*1024, 8*1024, 16*1024, 32*1024, 64*1024, 128*1024, 
                           256*1024, 512*1024, 1024*1024, 2*1024*1024, 4*1024*1024, 
                           8*1024*1024, 16*1024*1024, 32*1024*1024, 64*1024*1024, 
                           128*1024*1024, 256*1024*1024};
        
        for (uint64_t sz : sizes) {
            if (!quiet) {
                std::cout << "\n========================================\n";
                std::cout << "Testing size: ";
                if (sz >= 1024*1024) std::cout << (sz / (1024*1024)) << "M\n";
                else if (sz >= 1024) std::cout << (sz / 1024) << "K\n";
                else std::cout << sz << " bytes\n";
                std::cout << "========================================\n";
            }
            
            // Run simulation
            Cache l1d(32 * 1024, 64, 8, useLRU);
            Cache l2(256 * 1024, 64, 4, useLRU);
            Cache l3(8 * 1024 * 1024, 64, 12, useLRU);
            
            uint64_t totalAccesses = 0;
            uint64_t accessCount = sz / stride;
            uint64_t numPasses = (accessCount < 100) ? 10 : 
                                (accessCount < 1000) ? 5 : 2;
            
            if (randomAccess) {
                std::mt19937_64 gen(12345);
                std::uniform_int_distribution<uint64_t> dis(0, sz - stride);
                
                for (uint64_t pass = 0; pass < numPasses; pass++) {
                    for (uint64_t i = 0; i < accessCount; i++) {
                        uint64_t addr = dis(gen) & ~(stride - 1);
                        if (!l1d.access(addr)) {
                            if (!l2.access(addr)) {
                                l3.access(addr);
                            }
                        }
                        totalAccesses++;
                    }
                    gen.seed(12345);
                }
            } else {
                for (uint64_t pass = 0; pass < numPasses; pass++) {
                    for (uint64_t addr = 0; addr < sz; addr += stride) {
                        if (!l1d.access(addr)) {
                            if (!l2.access(addr)) {
                                l3.access(addr);
                            }
                        }
                        totalAccesses++;
                    }
                }
            }
            
            if (!quiet) {
                printResults(l1d, l2, l3, totalAccesses, !randomAccess);
            } else {
                std::cout << sz << "," << l1d.hitRate() << "," << l2.hitRate() << "," << l3.hitRate() << "\n";
            }
        }
        return 0;
    }

    // Create caches
    Cache l1d(32 * 1024, 64, 8, useLRU);
    Cache l2(256 * 1024, 64, 4, useLRU);
    Cache l3(8 * 1024 * 1024, 64, 12, useLRU);

    uint64_t totalAccesses = 0;
    uint64_t accessCount = testSize / stride;
    
    // For better hit rates, repeat the access pattern multiple times
    uint64_t numPasses = (accessCount < 100) ? 10 : 
                         (accessCount < 1000) ? 5 : 2;

    if (randomAccess) {
        // Random access pattern
        std::mt19937_64 gen(12345);
        std::uniform_int_distribution<uint64_t> dis(0, testSize - stride);

        for (uint64_t pass = 0; pass < numPasses; pass++) {
            for (uint64_t i = 0; i < accessCount; i++) {
                uint64_t addr = dis(gen) & ~(stride - 1);
                
                if (!l1d.access(addr)) {
                    if (!l2.access(addr)) {
                        l3.access(addr);
                    }
                }
                totalAccesses++;
            }
            gen.seed(12345);
        }
    } else {
        // Sequential access pattern - repeat multiple times
        for (uint64_t pass = 0; pass < numPasses; pass++) {
            for (uint64_t addr = 0; addr < testSize; addr += stride) {
                if (!l1d.access(addr)) {
                    if (!l2.access(addr)) {
                        l3.access(addr);
                    }
                }
                totalAccesses++;
            }
        }
    }

    if (!quiet) {
        printResults(l1d, l2, l3, totalAccesses, !randomAccess);
    }

    return 0;
}

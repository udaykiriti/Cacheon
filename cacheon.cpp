#include "sim.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

namespace {
    constexpr uint64_t DEFAULT_TEST_SIZE = 8 * 1024 * 1024;
    constexpr uint64_t DEFAULT_STRIDE = 64;
    constexpr uint64_t DEFAULT_PAGE_SIZE = 4096;

    CacheConfig defaultCache(uint64_t size, uint64_t lineSize, uint64_t associativity) {
        CacheConfig cfg;
        cfg.size = size;
        cfg.lineSize = lineSize;
        cfg.associativity = associativity;
        cfg.useLRU = false;
        cfg.writePolicy = WritePolicy::WriteBack;
        return cfg;
    }

    struct Options {
        uint64_t testSize = DEFAULT_TEST_SIZE;
        uint64_t stride = DEFAULT_STRIDE;
        bool randomAccess = false;
        bool quiet = false;
        bool runAllSizes = false;
        bool useLRU = false;
        uint64_t writeRate = 0;
        Prefetcher prefetcher = Prefetcher::None;
        WritePolicy writePolicy = WritePolicy::WriteBack;
        CacheConfig l1 = defaultCache(32 * 1024, 64, 8);
        CacheConfig l2 = defaultCache(256 * 1024, 64, 4);
        CacheConfig l3 = defaultCache(8 * 1024 * 1024, 64, 12);
        TlbConfig tlb{0, DEFAULT_PAGE_SIZE, true};
    };

    void printUsage() {
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
        std::cout << "  --l1 SIZE,LINE,ASSOC   Override L1 config (e.g., 32K,64,8)\n";
        std::cout << "  --l2 SIZE,LINE,ASSOC   Override L2 config\n";
        std::cout << "  --l3 SIZE,LINE,ASSOC   Override L3 config\n";
        std::cout << "  --write-policy wb|wt   Write-back or write-through\n";
        std::cout << "  --write-rate N         Percent writes (0-100)\n";
        std::cout << "  --prefetch none|next|stride   Prefetcher mode\n";
        std::cout << "  --tlb-entries N        Enable TLB with N entries\n";
        std::cout << "  --page-size SIZE       TLB page size (default 4K)\n";
        std::cout << "  -h, --help        Show this help\n";
        std::cout << "  -v, --version     Show version\n\n";
        std::cout << "Examples:\n";
        std::cout << "  cacheon 8M 64\n";
        std::cout << "  cacheon 128M 64 -r\n";
        std::cout << "  cacheon 256M 256 -Hr\n";
        std::cout << "  cacheon 4M 64 -l\n";
        std::cout << "  cacheon --all-sizes\n";
    }

    CacheConfig parseCacheSpec(const char *arg, const CacheConfig &base) {
        CacheConfig cfg = base;
        const std::string spec(arg ? arg : "");
        const size_t first = spec.find(',');
        const size_t second = (first == std::string::npos) ? std::string::npos : spec.find(',', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            throw std::runtime_error("Cache spec must be SIZE,LINE,ASSOC");
        }

        cfg.size = parseSize(spec.substr(0, first).c_str());
        cfg.lineSize = parseSize(spec.substr(first + 1, second - first - 1).c_str());
        cfg.associativity = std::strtoull(spec.substr(second + 1).c_str(), nullptr, 10);
        if (cfg.size == 0 || cfg.lineSize == 0 || cfg.associativity == 0) {
            throw std::runtime_error("Cache spec values must be non-zero");
        }
        return cfg;
    }

    WritePolicy parseWritePolicy(const char *arg) {
        const std::string value(arg ? arg : "");
        std::string lower;
        lower.reserve(value.size());
        for (char c : value) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower == "wb" || lower == "write-back" || lower == "writeback") {
            return WritePolicy::WriteBack;
        }
        if (lower == "wt" || lower == "write-through" || lower == "writethrough") {
            return WritePolicy::WriteThrough;
        }
        throw std::runtime_error("Unknown write policy: " + value);
    }

    Prefetcher parsePrefetcher(const char *arg) {
        const std::string value(arg ? arg : "");
        std::string lower;
        lower.reserve(value.size());
        for (char c : value) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower == "none") {
            return Prefetcher::None;
        }
        if (lower == "next" || lower == "nextline") {
            return Prefetcher::NextLine;
        }
        if (lower == "stride") {
            return Prefetcher::Stride;
        }
        throw std::runtime_error("Unknown prefetcher: " + value);
    }

    void printSizeHeader(uint64_t size) {
        std::cout << "\n========================================\n";
        std::cout << "Testing size: ";
        if (size >= 1024 * 1024)
            std::cout << (size / (1024 * 1024)) << "M\n";
        else if (size >= 1024)
            std::cout << (size / 1024) << "K\n";
        else
            std::cout << size << " bytes\n";
        std::cout << "========================================\n";
    }

    Options parseOptions(int argc, char *argv[]) {
        Options options;
        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
                printUsage();
                std::exit(0);
            }
            if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--version") == 0) {
                std::cout << "Cacheon 1.1\n";
                std::exit(0);
            }
            if (std::strcmp(argv[i], "--all-sizes") == 0) {
                options.runAllSizes = true;
                continue;
            }
            if (std::strcmp(argv[i], "-q") == 0 || std::strcmp(argv[i], "--quiet") == 0) {
                options.quiet = true;
                continue;
            }
            if (std::strcmp(argv[i], "-r") == 0 || std::strcmp(argv[i], "--random") == 0) {
                options.randomAccess = true;
                continue;
            }
            if (std::strcmp(argv[i], "-l") == 0 || std::strcmp(argv[i], "--lru") == 0) {
                options.useLRU = true;
                continue;
            }
            if (std::strcmp(argv[i], "--l1") == 0) {
                if (i + 1 >= argc)
                    throw std::runtime_error("--l1 requires SIZE,LINE,ASSOC");
                options.l1 = parseCacheSpec(argv[++i], options.l1);
                continue;
            }
            if (std::strcmp(argv[i], "--l2") == 0) {
                if (i + 1 >= argc)
                    throw std::runtime_error("--l2 requires SIZE,LINE,ASSOC");
                options.l2 = parseCacheSpec(argv[++i], options.l2);
                continue;
            }
            if (std::strcmp(argv[i], "--l3") == 0) {
                if (i + 1 >= argc)
                    throw std::runtime_error("--l3 requires SIZE,LINE,ASSOC");
                options.l3 = parseCacheSpec(argv[++i], options.l3);
                continue;
            }
            if (std::strcmp(argv[i], "--write-policy") == 0) {
                if (i + 1 >= argc)
                    throw std::runtime_error("--write-policy requires a value");
                options.writePolicy = parseWritePolicy(argv[++i]);
                continue;
            }
            if (std::strcmp(argv[i], "--write-back") == 0) {
                options.writePolicy = WritePolicy::WriteBack;
                continue;
            }
            if (std::strcmp(argv[i], "--write-through") == 0) {
                options.writePolicy = WritePolicy::WriteThrough;
                continue;
            }
            if (std::strcmp(argv[i], "--write-rate") == 0) {
                if (i + 1 >= argc)
                    throw std::runtime_error("--write-rate requires a value");
                options.writeRate = std::strtoull(argv[++i], nullptr, 10);
                if (options.writeRate > 100) {
                    throw std::runtime_error("write-rate must be 0-100");
                }
                continue;
            }
            if (std::strcmp(argv[i], "--prefetch") == 0) {
                if (i + 1 >= argc)
                    throw std::runtime_error("--prefetch requires a value");
                options.prefetcher = parsePrefetcher(argv[++i]);
                continue;
            }
            if (std::strcmp(argv[i], "--tlb-entries") == 0) {
                if (i + 1 >= argc)
                    throw std::runtime_error("--tlb-entries requires a value");
                options.tlb.entries = std::strtoull(argv[++i], nullptr, 10);
                continue;
            }
            if (std::strcmp(argv[i], "--page-size") == 0) {
                if (i + 1 >= argc)
                    throw std::runtime_error("--page-size requires a value");
                options.tlb.pageSize = parseSize(argv[++i]);
                continue;
            }
            if (std::strcmp(argv[i], "-H") == 0 || std::strcmp(argv[i], "--hugepage") == 0) {
                continue;
            }
            if (std::strcmp(argv[i], "-Hr") == 0 || std::strcmp(argv[i], "-rH") == 0) {
                options.randomAccess = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (i == 1) {
                    options.testSize = parseSize(argv[i]);
                }
                else if (i == 2) {
                    options.stride = parseSize(argv[i]);
                }
            }
        }
        options.l1.useLRU = options.useLRU;
        options.l2.useLRU = options.useLRU;
        options.l3.useLRU = options.useLRU;
        options.l1.writePolicy = options.writePolicy;
        options.l2.writePolicy = options.writePolicy;
        options.l3.writePolicy = options.writePolicy;
        return options;
    }

    void validateCacheConfig(const CacheConfig &cfg, const char *name) {
        if (cfg.lineSize == 0 || cfg.associativity == 0) {
            throw std::runtime_error(std::string(name) + " line size/associativity must be non-zero");
        }
        const uint64_t denom = cfg.lineSize * cfg.associativity;
        if (cfg.size < denom || (cfg.size % denom) != 0) {
            throw std::runtime_error(std::string(name) + " size must be a multiple of lineSize*associativity");
        }
    }

    void runForSize(uint64_t size, const Options &options) {
        if (!options.quiet) {
            printSizeHeader(size);
        }

        Sim l1d(options.l1);
        Sim l2(options.l2);
        Sim l3(options.l3);
        Tlb tlb(options.tlb);
        Tlb *tlbPtr = options.tlb.entries ? &tlb : nullptr;

        uint64_t totalAccesses = 0;
        const uint64_t numPasses = passCount(size / options.stride);

        runSimulation(l1d, l2, l3, tlbPtr, size, options.stride, options.randomAccess, numPasses,
                      options.writeRate, options.prefetcher, totalAccesses);

        if (!options.quiet) {
            printResults(l1d, l2, l3, tlbPtr, totalAccesses, !options.randomAccess);
        }
        else {
            std::cout << size << "," << l1d.hitRate() << "," << l2.hitRate() << "," << l3.hitRate() << "\n";
        }
    }
}

int main(int argc, char *argv[]) {
    try {
        const Options options = parseOptions(argc, argv);

        // Input validation
        if (options.stride == 0) {
            std::cerr << "Error: Stride cannot be zero\n";
            return 1;
        }
        if (options.stride > options.testSize) {
            std::cerr << "Error: Stride (" << options.stride << ") cannot exceed test size (" << options.testSize << ")\n";
            return 1;
        }
        validateCacheConfig(options.l1, "L1");
        validateCacheConfig(options.l2, "L2");
        validateCacheConfig(options.l3, "L3");
        if (options.tlb.entries > 0 && options.tlb.pageSize == 0) {
            std::cerr << "Error: TLB page size must be non-zero\n";
            return 1;
        }

        // Run full benchmark if requested
        if (options.runAllSizes) {
            const uint64_t sizes[] = {4 * 1024, 8 * 1024, 16 * 1024, 32 * 1024, 64 * 1024, 128 * 1024,
                                      256 * 1024, 512 * 1024, 1024 * 1024, 2 * 1024 * 1024, 4 * 1024 * 1024,
                                      8 * 1024 * 1024, 16 * 1024 * 1024, 32 * 1024 * 1024, 64 * 1024 * 1024,
                                      128 * 1024 * 1024, 256 * 1024 * 1024};

            for (const uint64_t sz : sizes) {
                runForSize(sz, options);
            }
            return 0;
        }

        runForSize(options.testSize, options);

        return 0;
    }
    catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}

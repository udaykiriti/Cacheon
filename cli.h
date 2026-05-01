#ifndef CACHEON_CLI_H
#define CACHEON_CLI_H

#include "sim.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace cli {

// Default values each can be overridden at compile time
#ifndef CACHEON_DEFAULT_TEST_SIZE
    #define CACHEON_DEFAULT_TEST_SIZE 0x800000ULL // 8MB
#endif
#ifndef CACHEON_DEFAULT_STRIDE
    #define CACHEON_DEFAULT_STRIDE 0x40 // 64 bytes
#endif
#ifndef CACHEON_HUGEPAGE_SIZE
    #define CACHEON_HUGEPAGE_SIZE 0x200000ULL // 2MB
#endif
constexpr uint64_t DEFAULT_TEST_SIZE = CACHEON_DEFAULT_TEST_SIZE;
constexpr uint64_t DEFAULT_STRIDE    = CACHEON_DEFAULT_STRIDE;
constexpr uint64_t HUGEPAGE_SIZE     = CACHEON_HUGEPAGE_SIZE;
constexpr uint64_t HUGEPAGE_BITS     = __builtin_ctzll(HUGEPAGE_SIZE); // log2(2MB) = 21

struct Options {
    uint64_t    testSize       = DEFAULT_TEST_SIZE;
    uint64_t    stride         = DEFAULT_STRIDE;
    bool        randomAccess   = false;
    bool        quiet          = false;
    bool        runAllSizes    = false;
    bool        useLRU         = false;
    bool        helpRequested  = false;
    bool        versionRequested = false;
    uint64_t    writeRate      = 0;
    Prefetcher  prefetcher     = Prefetcher::None;
    WritePolicy writePolicy    = WritePolicy::WriteBack;
    CacheConfig l1{0x8000,   0x40, 8,  false, WritePolicy::WriteBack};
    CacheConfig l2{0x40000,  0x40, 4,  false, WritePolicy::WriteBack};
    CacheConfig l3{0x800000, 0x40, 16, false, WritePolicy::WriteBack};
    TlbConfig   tlb{};
};

inline void printUsage() {
    std::cout << R"(Usage: cacheon [SIZE] [STRIDE] [OPTIONS]

SIZE     Memory size to test (default: 8M)
         Examples: 4K, 8M, 256M, 1G

STRIDE   Stride in bytes (default: 64)

OPTIONS:
  -H, --hugepage    Use hugepage-like mode (sets TLB page size to 2MB)
  -r, --random      Random access pattern
  -Hr, -rH          Both hugepage and random
  -q, --quiet       Quiet mode (CSV output: size,l1%,l2%,l3%)
  -l, --lru         Use LRU replacement (default: FIFO)
  --all-sizes       Run full benchmark (4K to 256M)
  --l1 SIZE,LINE,ASSOC   Override L1 config (e.g., 32K,64,8)
  --l2 SIZE,LINE,ASSOC   Override L2 config
  --l3 SIZE,LINE,ASSOC   Override L3 config
  --write-policy wb|wt   Write-back or write-through
  --write-rate N         Percent writes (0-100)
  --prefetch none|next|stride   Prefetcher mode
  --tlb-entries N        Enable TLB with N entries
  --page-size SIZE       TLB page size (default 4K)
  -h, --help        Show this help
  -v, --version     Show version

Examples:
  cacheon 8M 64
  cacheon 128M 64 -r
  cacheon 256M 256 -Hr
  cacheon 4M 64 -l
  cacheon --all-sizes
)";
}

inline CacheConfig parseCacheSpec(const char *arg, const CacheConfig &base) {
    CacheConfig cfg = base;
    const std::string spec(arg ? arg : "");
    const size_t first  = spec.find(',');
    const size_t second = (first == std::string::npos) ? std::string::npos : spec.find(',', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        throw std::runtime_error("Cache spec must be SIZE,LINE,ASSOC");
    }

    cfg.size          = parseSize(spec.substr(0, first).c_str());
    cfg.lineSize      = parseSize(spec.substr(first + 1, second - first - 1).c_str());
    cfg.associativity = std::strtoull(spec.substr(second + 1).c_str(), nullptr, 10);
    if (cfg.size == 0 || cfg.lineSize == 0 || cfg.associativity == 0) {
        throw std::runtime_error("Cache spec values must be non-zero");
    }
    return cfg;
}

inline std::string toLowerStr(const char *arg) {
    std::string s(arg ? arg : "");
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline WritePolicy parseWritePolicy(const char *arg) {
    const std::string lower = toLowerStr(arg);
    if (lower == "wb" || lower == "write-back"    || lower == "writeback")    return WritePolicy::WriteBack;
    if (lower == "wt" || lower == "write-through" || lower == "writethrough") return WritePolicy::WriteThrough;
    throw std::runtime_error(std::string("Unknown write policy: ") + (arg ? arg : ""));
}

inline Prefetcher parsePrefetcher(const char *arg) {
    const std::string lower = toLowerStr(arg);
    if (lower == "none")                         return Prefetcher::None;
    if (lower == "next" || lower == "nextline")  return Prefetcher::NextLine;
    if (lower == "stride")                       return Prefetcher::Stride;
    throw std::runtime_error(std::string("Unknown prefetcher: ") + (arg ? arg : ""));
}

inline std::string formatSize(uint64_t size) {
    if (size >= 1024 * 1024) return std::to_string(size / (1024 * 1024)) + "M";
    if (size >= 1024)        return std::to_string(size / 1024) + "K";
    return std::to_string(size) + " bytes";
}

inline void printSizeHeader(uint64_t size) {
    const std::string sep(40, '=');
    std::cout << "\n" << sep << "\nTesting size: " << formatSize(size) << "\n" << sep << "\n";
}

inline Options parseOptions(int argc, char *argv[]) {
    Options options;
    int positionalCount = 0;
    int i = 1;
    auto requireNextArg = [&](std::string_view flag) -> const char * {
        if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " requires a value");
        return argv[++i];
    };

    for (; i < argc; i++) {
        const std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help")    { options.helpRequested    = true; return options; }
        if (arg == "-v" || arg == "--version") { options.versionRequested = true; return options; }

        if (arg == "--all-sizes")              { options.runAllSizes   = true; continue; }
        if (arg == "-q" || arg == "--quiet")   { options.quiet         = true; continue; }
        if (arg == "-r" || arg == "--random")  { options.randomAccess  = true; continue; }
        if (arg == "-l" || arg == "--lru")     { options.useLRU        = true; continue; }

        if (arg == "--l1")           { options.l1 = parseCacheSpec(requireNextArg(arg), options.l1); continue; }
        if (arg == "--l2")           { options.l2 = parseCacheSpec(requireNextArg(arg), options.l2); continue; }
        if (arg == "--l3")           { options.l3 = parseCacheSpec(requireNextArg(arg), options.l3); continue; }
        if (arg == "--write-policy") { options.writePolicy = parseWritePolicy(requireNextArg(arg)); continue; }
        if (arg == "--prefetch")     { options.prefetcher  = parsePrefetcher(requireNextArg(arg));  continue; }
        if (arg == "--page-size") {
            options.tlb.pageSize = parseSize(requireNextArg(arg));
            options.tlb.pageBits = static_cast<uint64_t>(__builtin_ctzll(options.tlb.pageSize));
            continue;
        }
        if (arg == "--tlb-entries")  { options.tlb.entries  = std::strtoull(requireNextArg(arg), nullptr, 10); continue; }

        if (arg == "--write-rate") {
            options.writeRate = std::strtoull(requireNextArg(arg), nullptr, 10);
            if (options.writeRate > 100) throw std::runtime_error("write-rate must be 0-100");
            continue;
        }

        if (arg == "-H" || arg == "--hugepage") { options.tlb.pageSize = HUGEPAGE_SIZE; options.tlb.pageBits = HUGEPAGE_BITS; continue; }
        if (arg == "-Hr" || arg == "-rH")       { options.randomAccess = true; options.tlb.pageSize = HUGEPAGE_SIZE; options.tlb.pageBits = HUGEPAGE_BITS; continue; }

        if (arg[0] != '-') {
            if      (positionalCount == 0) { options.testSize = parseSize(argv[i]); positionalCount++; }
            else if (positionalCount == 1) { options.stride   = parseSize(argv[i]); positionalCount++; }
        } else {
            throw std::runtime_error("Unknown option: " + std::string(arg));
        }
    }
    options.l1.useLRU = options.l2.useLRU = options.l3.useLRU = options.useLRU;
    options.l1.writePolicy = options.l2.writePolicy = options.l3.writePolicy = options.writePolicy;
    return options;
}

inline void validateCacheConfig(const CacheConfig &cfg, const char *name) {
    if (cfg.size == 0 || cfg.lineSize == 0 || cfg.associativity == 0) {
        throw std::runtime_error(std::string(name) + " size, line size, and associativity must be non-zero");
    }
    const uint64_t denom = cfg.lineSize * cfg.associativity;
    if (cfg.size % denom != 0) {
        throw std::runtime_error(std::string(name) + " size must be a multiple of lineSize*associativity");
    }
}

inline void runForSize(uint64_t size, const Options &options) {
    ScopedTimer timer(options.quiet);
    if (!options.quiet) printSizeHeader(size);

    Sim l1d(options.l1);
    Sim l2(options.l2);
    Sim l3(options.l3);

    std::optional<Tlb> tlb = options.tlb.entries ? std::optional<Tlb>(options.tlb) : std::nullopt;
    Tlb *tlbPtr = tlb ? &*tlb : nullptr;

    uint64_t totalAccesses = 0;
    const uint64_t numPasses = passCount(size / options.stride);

    runSimulation(l1d, l2, l3, tlbPtr, size, options.stride, options.randomAccess,
                  numPasses, options.writeRate, options.prefetcher, totalAccesses);

    if (!options.quiet) {
        printResults(l1d, l2, l3, tlbPtr, totalAccesses, !options.randomAccess);
    } else {
        std::cout << size << "," << l1d.hitRate() << "," << l2.hitRate() << "," << l3.hitRate() << "\n";
    }
}

} // namespace cli

#endif // CACHEON_CLI_H

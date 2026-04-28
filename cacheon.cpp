#include "cli.h"

int main(int argc, char *argv[]) {
    try {
        const cli::Options options = cli::parseOptions(argc, argv);

        if (options.helpRequested)    { cli::printUsage(); return 0; }
        if (options.versionRequested) { std::cout << "Cacheon " CACHEON_VERSION "\n"; return 0; }

        if (options.stride == 0) {
            std::cerr << "Error: Stride cannot be zero\n";
            return 1;
        }
        if (options.stride > options.testSize) {
            std::cerr << "Error: Stride (" << options.stride
                      << ") cannot exceed test size (" << options.testSize << ")\n";
            return 1;
        }
        if (options.randomAccess && (options.stride & (options.stride - 1)) != 0) {
            std::cerr << "Error: Random access mode requires a power-of-two stride (e.g. 64, 128, 256)\n";
            return 1;
        }
        cli::validateCacheConfig(options.l1, "L1");
        cli::validateCacheConfig(options.l2, "L2");
        cli::validateCacheConfig(options.l3, "L3");
        
        if (options.tlb.entries > 0 && options.tlb.pageSize == 0) {
            std::cerr << "Error: TLB page size must be non-zero\n";
            return 1;
        }

        if (options.runAllSizes) {
            constexpr std::array<uint64_t, 17> sizes = {
                0x1000, 0x2000, 0x4000, 0x8000, 0x10000, 0x20000,
                0x40000, 0x80000, 0x100000, 0x200000, 0x400000,
                0x800000, 0x1000000, 0x2000000, 0x4000000,
                0x8000000, 0x10000000
            };
            for (const uint64_t sz : sizes) {
                cli::runForSize(sz, options);
            }
            return 0;
        }

        cli::runForSize(options.testSize, options);
        return 0;
    }
    catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}

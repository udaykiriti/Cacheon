#include "sim.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
  // Cycle costs sourced from CACHEON_CYCLES_* defines in sim.h (overridable at compile time)
  constexpr uint64_t CYCLES_L1     = CACHEON_CYCLES_L1;
  constexpr uint64_t CYCLES_L2     = CACHEON_CYCLES_L2;
  constexpr uint64_t CYCLES_L3     = CACHEON_CYCLES_L3;
  constexpr uint64_t CYCLES_MEMORY = CACHEON_CYCLES_MEMORY;

  constexpr uint64_t RANDOM_SEED = 12345;
  constexpr uint64_t WRITE_SEED  = 67890;

  struct PrefetchState {
    bool     hasLast   = false;
    uint64_t lastAddr  = 0;
    uint64_t lastStride = 0;
  };

  void accessAll(Sim &l1d, Sim &l2, Sim &l3, uint64_t addr, bool isWrite, bool isPrefetch) {
    if (!l1d.access(addr, isWrite, isPrefetch)) {
      if (!l2.access(addr, isWrite, isPrefetch)) {
        (void)l3.access(addr, isWrite, isPrefetch);
      }
    }
  }

} // namespace

ScopedTimer::ScopedTimer(bool q) : start(Clock::now()), quiet(q) {}

ScopedTimer::~ScopedTimer() {
  if (!quiet) {
    auto end = Clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n[Execution Time: " << ms << " ms]\n";
  }
}

Sim::Sim(const CacheConfig &cfg) : config(cfg) {
  const std::size_t numSets =
      static_cast<std::size_t>(config.size / (config.lineSize * config.associativity));
  sets.resize(numSets);
  for (auto& set : sets) {
    set.blocks.reserve(config.associativity);
  }
  faCapacity = config.lineSize ? (config.size / config.lineSize) : 0;

  if (config.lineSize > 0 && numSets > 0 && 
      (config.lineSize & (config.lineSize - 1)) == 0 && 
      (numSets & (numSets - 1)) == 0) {
    useBitwise = true;
    lineShift = __builtin_ctzll(config.lineSize);
    setMask   = numSets - 1;
    tagShift  = lineShift + __builtin_ctzll(numSets);
  }
}

bool Sim::access(uint64_t addr, bool isWrite, bool isPrefetch) {
  const std::size_t numSets = sets.size();
  if (numSets == 0) return false;

  std::size_t setIndex;
  uint64_t tag;
  if (useBitwise) {
    setIndex = (addr >> lineShift) & setMask;
    tag      = addr >> tagShift;
  } else {
    setIndex = static_cast<std::size_t>((addr / config.lineSize) % numSets);
    tag      = addr / (config.lineSize * numSets);
  }

  auto &set = sets[setIndex];

  // --- Cache hit ---
  if (set.contains(tag)) {
    isPrefetch ? stats.prefetchHits++ : stats.demandHits++;

    if (config.useLRU) {
      set.promote(tag);
    }

    if (isWrite) {
      if (config.writePolicy == WritePolicy::WriteBack) {
        set.setDirty(tag);
      } else {
        stats.writeThroughWrites++;
      }
    }

    if (faCapacity > 0) {
      faShadow.promote(tag);
    }
#ifdef CACHEON_DEBUG
    std::cerr << "[debug] HIT  set=" << setIndex << " tag=" << tag << "\n";
#endif
    return true;
  }

  // --- Cache miss ---
  if (isPrefetch) {
    stats.prefetchMisses++;
  } else {
    stats.demandMisses++;
    const bool isCold = seenTags.insert(tag).second;
    if (isCold) {
      stats.coldMisses++;
    } else if (faShadow.contains(tag)) {
      stats.conflictMisses++;
    } else {
      stats.capacityMisses++;
    }
#ifdef CACHEON_DEBUG
    const char *missType = isCold ? "cold" : faShadow.contains(tag) ? "conflict" : "capacity";
    std::cerr << "[debug] MISS set=" << setIndex << " tag=" << tag << " " << missType << "\n";
#endif
  }

  // Evict if the set is full
  if (set.size() >= config.associativity) {
    bool wasDirty = false;
    set.evict(&wasDirty);
    
    if (wasDirty && config.writePolicy == WritePolicy::WriteBack) {
      stats.dirtyEvictions++;
      stats.writeBacks++;
    }
  }

  // Insert new tag
  const bool isDirtyWrite = isWrite && (config.writePolicy == WritePolicy::WriteBack);
  set.insert(tag, isDirtyWrite);
  
  if (isWrite && !isDirtyWrite) {
    stats.writeThroughWrites++;
  }

  // Update fully-associative shadow for conflict-miss classification
  if (faCapacity > 0) {
    if (faShadow.contains(tag)) {
      faShadow.promote(tag);
    } else {
      if (faShadow.size() >= faCapacity) {
        faShadow.evict();
      }
      faShadow.insert(tag);
    }
  }
  return false;
}

double Sim::hitRate() const {
  const uint64_t total = stats.demandHits + stats.demandMisses;
  return total ? (100.0 * stats.demandHits / total) : 0.0;
}

std::ostream& operator<<(std::ostream& os, const Sim& sim) {
  const double rate = sim.hitRate();
  os << "  " << makeBar(rate) << " " << std::fixed << std::setprecision(2) << rate << "%\n"
     << "  Demand Hits: "   << sim.stats.demandHits   << " | Demand Misses: "   << sim.stats.demandMisses   << "\n"
     << "  Prefetch Hits: " << sim.stats.prefetchHits << " | Prefetch Misses: " << sim.stats.prefetchMisses << "\n"
     << "  Miss Breakdown: cold " << sim.stats.coldMisses 
     << ", conflict " << sim.stats.conflictMisses 
     << ", capacity " << sim.stats.capacityMisses << "\n"
     << "  Write-Backs: " << sim.stats.writeBacks 
     << " | Dirty Evictions: " << sim.stats.dirtyEvictions 
     << " | Write-Through Writes: " << sim.stats.writeThroughWrites << "\n\n";
  return os;
}

Tlb::Tlb(const TlbConfig &cfg) : config(cfg) {}

bool Tlb::access(uint64_t addr) {
  // Caller must only invoke this when entries > 0 and pageSize > 0
  assert(config.entries > 0 && config.pageSize > 0);

  const uint64_t tag = addr >> config.pageBits;

  if (store.contains(tag)) {
    stats.hits++;
    if (config.useLRU) {
      store.promote(tag);
    }
    return true;
  }

  stats.misses++;
  if (store.size() >= config.entries) {
    store.evict();
  }
  store.insert(tag);
  return false;
}

std::ostream& operator<<(std::ostream& os, const Tlb& tlb) {
  if (tlb.config.entries == 0) return os;
  const uint64_t total = tlb.stats.hits + tlb.stats.misses;
  const double rate = total ? (100.0 * tlb.stats.hits / total) : 0.0;
  os << "TLB Sim:\n"
     << "  " << makeBar(rate) << " " << std::fixed << std::setprecision(2) << rate << "%\n"
     << "  Hits: " << tlb.stats.hits << " | Misses: " << tlb.stats.misses << "\n\n";
  return os;
}

uint64_t passCount(uint64_t accessCount) {
  if (accessCount < 100)  return 10;
  if (accessCount < 1000) return 5;
  return 2;
}

void runSimulation(Sim &l1d, Sim &l2, Sim &l3, Tlb *tlb, uint64_t size, uint64_t stride,
                   bool randomAccess, uint64_t numPasses, uint64_t writeRate,
                   Prefetcher prefetcher, uint64_t &totalAccesses) {
  const uint64_t accessCount = size / stride;
  std::mt19937_64 addrGen(RANDOM_SEED);
  std::mt19937_64 writeGen(WRITE_SEED);
  std::uniform_int_distribution<uint64_t> addrDis(0, size - stride);
  std::uniform_int_distribution<uint64_t> writeDis(0, 99);
  PrefetchState prefetchState;

  auto recordAccess = [&](uint64_t addr, bool isWrite, bool isPrefetch) {
    if (tlb && !isPrefetch) {
      (void)tlb->access(addr);
    }
    accessAll(l1d, l2, l3, addr, isWrite, isPrefetch);
  };

  auto maybePrefetch = [&](uint64_t addr) {
    if (prefetcher == Prefetcher::None) return;

    if (prefetcher == Prefetcher::NextLine) {
      recordAccess(addr + l1d.config.lineSize, false, true);
      return;
    }

    if (prefetcher == Prefetcher::Stride) {
      if (prefetchState.hasLast) {
        const uint64_t strideValue = addr - prefetchState.lastAddr;
        if (strideValue != 0 && strideValue == prefetchState.lastStride) {
          recordAccess(addr + strideValue, false, true);
        }
        prefetchState.lastStride = strideValue;
      }
      prefetchState.lastAddr = addr;
      prefetchState.hasLast  = true;
    }
  };

  const bool doWrites = writeRate > 0;

  for (uint64_t pass = 0; pass < numPasses; pass++) {
    for (uint64_t i = 0; i < accessCount; i++) {
      const uint64_t addr    = randomAccess ? (addrDis(addrGen) & ~(stride - 1)) : (i * stride);
      const bool     isWrite = doWrites && writeDis(writeGen) < writeRate;
      recordAccess(addr, isWrite, false);
      maybePrefetch(addr);
    }
    totalAccesses += accessCount;
    // Don't reseed for random access — each pass should use a different sequence,
    // not repeat the same addresses (the old reseed made all passes identical).
  }
}

std::string makeBar(double percentage, int width) {
  const int filled = static_cast<int>(percentage / 100.0 * width);
  return '[' + std::string(static_cast<std::size_t>(filled), '=')
             + std::string(static_cast<std::size_t>(width - filled), ' ')
             + ']';
}

uint64_t parseSize(const char *str) {
  if (!str || *str == '\0') {
    throw std::runtime_error("Empty size string");
  }

  char *endptr = nullptr;
  const uint64_t num = std::strtoull(str, &endptr, 10);

  if (endptr == str) {
    throw std::runtime_error(std::string("Invalid size '") + str + "'");
  }

  if (*endptr == '\0') return num;

  if (*(endptr + 1) != '\0') {
    throw std::runtime_error(std::string("Invalid size '") + str + "'");
  }

  switch (std::tolower(static_cast<unsigned char>(*endptr))) {
  case 'k': return num * 1024ULL;
  case 'm': return num * 1024ULL * 1024ULL;
  case 'g': return num * 1024ULL * 1024ULL * 1024ULL;
  default:
    throw std::runtime_error(std::string("Unknown suffix '") + *endptr + "' in size");
  }
}

void printResults(const Sim &l1d, const Sim &l2, const Sim &l3, const Tlb *tlb,
                  uint64_t totalAccesses, bool sequential) {
  const double l1Hit = l1d.hitRate();
  const double l2Hit = l2.hitRate();
  const double l3Hit = l3.hitRate();

  const double l1Miss = (100.0 - l1Hit) / 100.0;
  const double l2Miss = (100.0 - l2Hit) / 100.0;
  const double l3Miss = (100.0 - l3Hit) / 100.0;

  // AMAT = L1_hit_time + L1_miss_rate*(L2_hit_time + L2_miss_rate*(L3_hit_time + L3_miss_rate*mem))
  const double amat = CYCLES_L1     * (l1Hit / 100.0)
                    + CYCLES_L2     * l1Miss * (l2Hit / 100.0)
                    + CYCLES_L3     * l1Miss * l2Miss * (l3Hit / 100.0)
                    + CYCLES_MEMORY * l1Miss * l2Miss * l3Miss;

  std::cout << "\nCACHE REPORT\n============\n\n"
            << "Total Accesses: " << totalAccesses << "\n"
            << "Pattern: " << (sequential ? "Sequential" : "Random") << "\n\n"
            << "L1D Sim:\n" << l1d
            << "L2 Sim:\n"  << l2
            << "L3 Sim:\n"  << l3;

  if (tlb) {
    std::cout << *tlb;
  }

  std::cout << "AMAT (Average Memory Access Time): "
            << std::fixed << std::setprecision(2) << amat << " cycles\n";
}

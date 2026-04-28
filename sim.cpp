#include "sim.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
  constexpr uint64_t CYCLES_L1 = 4;
  constexpr uint64_t CYCLES_L2 = 12;
  constexpr uint64_t CYCLES_L3 = 40;
  constexpr uint64_t CYCLES_MEMORY = 200;

  constexpr uint64_t RANDOM_SEED = 12345;
  constexpr uint64_t WRITE_SEED = 67890;

  struct PrefetchState {
    bool hasLast = false;
    uint64_t lastAddr = 0;
    uint64_t lastStride = 0;
  };

  void updateOrder(std::deque<uint64_t> &order, uint64_t tag) {
    auto it = std::find(order.begin(), order.end(), tag);
    if (it != order.end()) {
      order.erase(it);
      order.push_back(tag);
    }
  }
}

Sim::Sim(const CacheConfig &cfg): config(cfg) {
  const std::size_t numSets = static_cast<std::size_t>(config.size / (config.lineSize * config.associativity));
  sets.resize(numSets);
  setOrder.resize(numSets);
  faCapacity = config.lineSize ? (config.size / config.lineSize) : 0;
}

bool Sim::access(uint64_t addr, bool isWrite, bool isPrefetch) {
  const std::size_t numSets = sets.size();
  if (numSets == 0) {
    return false;
  }

  const std::size_t setIndex = static_cast<std::size_t>((addr / config.lineSize) % numSets);
  const uint64_t tag = addr / (config.lineSize * numSets);
  auto &set = sets[setIndex];
  auto &order = setOrder[setIndex];

  auto it = set.find(tag);
  if (it != set.end()) {
    if (isPrefetch) {
      stats.prefetchHits++;
    }
    else {
      stats.demandHits++;
    }

    if (config.useLRU) {
      updateOrder(order, tag);
    }

    if (isWrite) {
      if (config.writePolicy == WritePolicy::WriteBack) {
        it->second = true;
      }
      else {
        stats.writeThroughWrites++;
      }
    }

    if (faCapacity > 0) {
      updateOrder(faOrder, tag);
    }
    return true;
  }

  if (isPrefetch) {
    stats.prefetchMisses++;
  }
  else {
    stats.demandMisses++;
    const bool isCold = seenTags.insert(tag).second;
    if (isCold) {
      stats.coldMisses++;
    }
    else if (faTags.count(tag)) {
      stats.conflictMisses++;
    }
    else {
      stats.capacityMisses++;
    }
  }

  if (set.size() >= config.associativity) {
    const uint64_t victim = order.front();
    order.pop_front();
    const bool victimDirty = set[victim];
    if (victimDirty && config.writePolicy == WritePolicy::WriteBack) {
      stats.dirtyEvictions++;
      stats.writeBacks++;
    }
    set.erase(victim);
  }

  const bool isDirty = isWrite && (config.writePolicy == WritePolicy::WriteBack);
  set[tag] = isDirty;
  order.push_back(tag);

  if (isWrite && config.writePolicy == WritePolicy::WriteThrough) {
    stats.writeThroughWrites++;
  }

  if (faCapacity > 0) {
    if (!faTags.insert(tag).second) {
      updateOrder(faOrder, tag);
    }
    else {
      if (faTags.size() > faCapacity) {
        const uint64_t faVictim = faOrder.front();
        faOrder.pop_front();
        faTags.erase(faVictim);
      }
      faOrder.push_back(tag);
    }
  }
  return false;
}

double Sim::hitRate() const {
  const uint64_t total = stats.demandHits + stats.demandMisses;
  return total ? (100.0 * stats.demandHits / total) : 0.0;
}

void Sim::reset() {
  stats = CacheStats{};
  for (auto &s : sets)
    s.clear();
  for (auto &o : setOrder)
    o.clear();
  seenTags.clear();
  faTags.clear();
  faOrder.clear();
}

Tlb::Tlb(const TlbConfig &cfg)
    : config(cfg) {
}

bool Tlb::access(uint64_t addr) {
  if (config.entries == 0 || config.pageSize == 0) {
    return true;
  }

  const uint64_t tag = addr / config.pageSize;
  if (tags.count(tag)) {
    stats.hits++;
    if (config.useLRU) {
      updateOrder(order, tag);
    }
    return true;
  }

  stats.misses++;
  if (tags.size() >= config.entries) {
    const uint64_t victim = order.front();
    order.pop_front();
    tags.erase(victim);
  }
  tags.insert(tag);
  order.push_back(tag);
  return false;
}

void Tlb::reset() {
  stats = TlbStats{};
  tags.clear();
  order.clear();
}

uint64_t passCount(uint64_t accessCount) {
  if (accessCount < 100) {
    return 10;
  }
  if (accessCount < 1000) {
    return 5;
  }
  return 2;
}

void accessAll(Sim &l1d, Sim &l2, Sim &l3, uint64_t addr, bool isWrite, bool isPrefetch) {
  if (!l1d.access(addr, isWrite, isPrefetch)) {
    if (!l2.access(addr, isWrite, isPrefetch)) {
      l3.access(addr, isWrite, isPrefetch);
    }
  }
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
      tlb->access(addr);
    }
    accessAll(l1d, l2, l3, addr, isWrite, isPrefetch);
  };

  auto maybePrefetch = [&](uint64_t addr) {
    if (prefetcher == Prefetcher::None) {
      return;
    }

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
      prefetchState.hasLast = true;
    }
  };

  for (uint64_t pass = 0; pass < numPasses; pass++) {
    for (uint64_t i = 0; i < accessCount; i++) {
      const uint64_t addr = randomAccess ? (addrDis(addrGen) & ~(stride - 1)) : (i * stride);
      const bool isWrite = writeRate > 0 && writeDis(writeGen) < writeRate;
      recordAccess(addr, isWrite, false);
      maybePrefetch(addr);
      totalAccesses++;
    }
    if (randomAccess) {
      addrGen.seed(RANDOM_SEED);
      writeGen.seed(WRITE_SEED);
    }
  }
}

std::string makeBar(double percentage, int width) {
  const int filled = static_cast<int>(percentage / 100.0 * width);
  std::string bar = "[";
  for (int i = 0; i < width; i++){
    bar += (i < filled) ? "=" : " ";
  }
  bar += "]";
  return bar;
}

uint64_t parseSize(const char *str) {
  if (!str || *str == '\0') {
    throw std::runtime_error("Empty size string");
  }

  char *endptr;
  const uint64_t num = std::strtoll(str, &endptr, 10);

  if (num == 0 || (endptr == str && *str != '0')) {
    throw std::runtime_error(std::string("Invalid size '") + str + "'");
  }

  if (!endptr || *endptr == '\0')
    return num;

  if (*(endptr + 1) != '\0') {
    throw std::runtime_error(std::string("Invalid size '") + str + "'");
  }

  switch (std::tolower(static_cast<unsigned char>(*endptr))) {
  case 'k':
    return num * 1024;
  case 'm':
    return num * 1024 * 1024;
  case 'g':
    return num * 1024 * 1024 * 1024;
  default:
    throw std::runtime_error(std::string("Unknown suffix '") + *endptr + "' in size");
  }
}

void printResults(const Sim &l1d, const Sim &l2, const Sim &l3, const Tlb *tlb,
                  uint64_t totalAccesses, bool sequential) {
  const double l1dRate = l1d.hitRate();
  const double l2Rate = l2.hitRate();
  const double l3Rate = l3.hitRate();

  const double amat = CYCLES_L1 * (l1dRate / 100.0) +
                      CYCLES_L2 * ((100.0 - l1dRate) / 100.0) * (l2Rate / 100.0) +
                      CYCLES_L3 * ((100.0 - l1dRate) / 100.0) * ((100.0 - l2Rate) / 100.0) * (l3Rate / 100.0) +
                      CYCLES_MEMORY * ((100.0 - l1dRate) / 100.0) * ((100.0 - l2Rate) / 100.0) * ((100.0 - l3Rate) / 100.0);

  std::cout << "\nCACHE REPORT\n";
  std::cout << "============\n\n";
  std::cout << "Total Accesses: " << totalAccesses << "\n";
  std::cout << "Pattern: " << (sequential ? "Sequential" : "Random") << "\n\n";

  auto printCache = [&](const char *label, const Sim &sim) {
    std::cout << label << "\n";
    std::cout << "  " << makeBar(sim.hitRate()) << " " << std::fixed << std::setprecision(2)
              << sim.hitRate() << "%\n";
    std::cout << "  Demand Hits: " << sim.stats.demandHits << " | Demand Misses: " << sim.stats.demandMisses << "\n";
    std::cout << "  Prefetch Hits: " << sim.stats.prefetchHits << " | Prefetch Misses: " << sim.stats.prefetchMisses << "\n";
    std::cout << "  Miss Breakdown: cold " << sim.stats.coldMisses
              << ", conflict " << sim.stats.conflictMisses
              << ", capacity " << sim.stats.capacityMisses << "\n";
    std::cout << "  Write-Backs: " << sim.stats.writeBacks << " | Dirty Evictions: " << sim.stats.dirtyEvictions
              << " | Write-Through Writes: " << sim.stats.writeThroughWrites << "\n\n";
  };

  printCache("L1D Sim:", l1d);
  printCache("L2 Sim:", l2);
  printCache("L3 Sim:", l3);

  if (tlb && tlb->config.entries > 0) {
    const uint64_t total = tlb->stats.hits + tlb->stats.misses;
    const double rate = total ? (100.0 * tlb->stats.hits / total) : 0.0;
    std::cout << "TLB:\n";
    std::cout << "  " << makeBar(rate) << " " << std::fixed << std::setprecision(2) << rate << "%\n";
    std::cout << "  Hits: " << tlb->stats.hits << " | Misses: " << tlb->stats.misses << "\n\n";
  }

  std::cout << "AMAT (Average Memory Access Time): " << std::fixed << std::setprecision(2) << amat << " cycles\n";
}

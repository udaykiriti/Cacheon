#ifndef CACHEON_SIM_H
#define CACHEON_SIM_H

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Version string  override with -DCACHEON_VERSION=\"x.y\" if needed
#ifndef CACHEON_VERSION
#  define CACHEON_VERSION "1.1"
#endif

// AMAT cycle costs  override at compile time e.g. -DCACHEON_CYCLES_L1=5
#ifndef CACHEON_CYCLES_L1
#  define CACHEON_CYCLES_L1      4
#endif
#ifndef CACHEON_CYCLES_L2
#  define CACHEON_CYCLES_L2     12
#endif
#ifndef CACHEON_CYCLES_L3
#  define CACHEON_CYCLES_L3     40
#endif
#ifndef CACHEON_CYCLES_MEMORY
#  define CACHEON_CYCLES_MEMORY 200
#endif

enum class WritePolicy {
  WriteBack,
  WriteThrough
};

enum class Prefetcher {
  None,
  NextLine,
  Stride
};

struct CacheConfig {
  uint64_t size = 0;
  uint64_t lineSize = 0;
  uint64_t associativity = 0;
  bool useLRU = false;
  WritePolicy writePolicy = WritePolicy::WriteBack;
};

struct CacheStats {
  uint64_t demandHits = 0;
  uint64_t demandMisses = 0;
  uint64_t prefetchHits = 0;
  uint64_t prefetchMisses = 0;
  uint64_t coldMisses = 0;
  uint64_t conflictMisses = 0;
  uint64_t capacityMisses = 0;
  uint64_t dirtyEvictions = 0;
  uint64_t writeBacks = 0;
  uint64_t writeThroughWrites = 0;
};

struct TlbConfig {
  uint64_t entries = 0;
  uint64_t pageSize = 4096;
  bool useLRU = true;
};

struct TlbStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
};

// O(1) LRU Set encapsulation
using LruList    = std::list<uint64_t>;
using LruIterMap = std::unordered_map<uint64_t, LruList::iterator>;

struct LRUSet {
  std::unordered_set<uint64_t> tags;
  std::unordered_set<uint64_t> dirtyBits; // Only used by Sim, ignored by TLB/FA
  LruList    order;
  LruIterMap iterMap;

  bool contains(uint64_t tag) const { return tags.count(tag); }
  
  void promote(uint64_t tag) {
    order.splice(order.end(), order, iterMap.at(tag));
  }
  
  void insert(uint64_t tag, bool isDirty = false) {
    tags.insert(tag);
    if (isDirty) dirtyBits.insert(tag);
    iterMap[tag] = order.insert(order.end(), tag);
  }
  
  uint64_t evict(bool* wasDirty = nullptr) {
    uint64_t victim = order.front();
    order.pop_front();
    iterMap.erase(victim);
    tags.erase(victim);
    if (wasDirty) {
      *wasDirty = dirtyBits.erase(victim);
    }
    return victim;
  }

  size_t size() const { return tags.size(); }
};

struct Sim {
  CacheConfig config;
  CacheStats  stats;

  std::vector<LRUSet> sets;
  std::unordered_set<uint64_t> seenTags;
  LRUSet faShadow;
  uint64_t faCapacity = 0;

  explicit Sim(const CacheConfig &cfg);

  [[nodiscard]] bool   access(uint64_t addr, bool isWrite, bool isPrefetch);
  [[nodiscard]] double hitRate() const;
};

struct Tlb {
  TlbConfig config;
  TlbStats  stats;

  LRUSet store;

  explicit Tlb(const TlbConfig &cfg);

  [[nodiscard]] bool access(uint64_t addr);
};

[[nodiscard]] uint64_t passCount(uint64_t accessCount);
void runSimulation(Sim &l1d, Sim &l2, Sim &l3, Tlb *tlb, uint64_t size, uint64_t stride,
                   bool randomAccess, uint64_t numPasses, uint64_t writeRate,
                   Prefetcher prefetcher, uint64_t &totalAccesses);

[[nodiscard]] std::string makeBar(double percentage, int width = 20);
[[nodiscard]] uint64_t    parseSize(const char *str);
void printResults(const Sim &l1d, const Sim &l2, const Sim &l3, const Tlb *tlb,
                  uint64_t totalAccesses, bool sequential);

#endif

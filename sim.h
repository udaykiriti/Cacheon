#ifndef CACHEON_SIM_H
#define CACHEON_SIM_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <list>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Version string  override with -DCACHEON_VERSION=\"x.y\" if needed
#ifndef CACHEON_VERSION
  #define CACHEON_VERSION "1.1"
#endif

// AMAT cycle costs  override at compile time e.g. -DCACHEON_CYCLES_L1=5
#ifndef CACHEON_CYCLES_L1
  #define CACHEON_CYCLES_L1      4
#endif
#ifndef CACHEON_CYCLES_L2
  #define CACHEON_CYCLES_L2     12
#endif
#ifndef CACHEON_CYCLES_L3
  #define CACHEON_CYCLES_L3     40
#endif
#ifndef CACHEON_CYCLES_MEMORY
  #define CACHEON_CYCLES_MEMORY 200
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

// RAII Timer for measuring execution time
class ScopedTimer {
  using Clock = std::chrono::high_resolution_clock;
  std::chrono::time_point<Clock> start;
  bool quiet;
public:
  explicit ScopedTimer(bool q = false);
  ~ScopedTimer();
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
  uint64_t entries  = 0;
  uint64_t pageSize = 4096;
  uint64_t pageBits = 12;   // log2(pageSize) - precomputed for bitwise shift
  bool useLRU = true;
};

struct TlbStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
};

// O(1) LRU Set encapsulation for large sets
using LruList    = std::list<uint64_t>;
using LruIterMap = std::unordered_map<uint64_t, LruList::iterator>;

struct HashLRUSet {
  std::unordered_set<uint64_t> tags;
  LruList    order;
  LruIterMap iterMap;

  bool contains(uint64_t tag) const { return tags.contains(tag); }

  void promote(uint64_t tag) {
    order.splice(order.end(), order, iterMap.at(tag));
  }

  void insert(uint64_t tag) {
    tags.insert(tag);
    iterMap[tag] = order.insert(order.end(), tag);
  }

  void evict() {
    uint64_t victim = order.front();
    order.pop_front();
    iterMap.erase(victim);
    tags.erase(victim);
  }

  size_t size() const { return tags.size(); }
};

// O(N) array-backed LRU Set for small cache sets (eliminates hash/heap overhead)
struct LinearLRUSet {
  struct Block {
    uint64_t tag;
    bool dirty;
  };
  std::vector<Block> blocks;

  bool contains(uint64_t tag) const {
    for (const auto& b : blocks) {
      if (b.tag == tag) return true;
    }
    return false;
  }

  // Fuses contains + promote into one scan: returns true and moves tag to MRU
  // position if found, false if not. Eliminates the double-scan on every hit.
  bool promoteIfContains(uint64_t tag) {
    for (auto it = blocks.begin(); it != blocks.end(); ++it) {
      if (it->tag == tag) {
        Block saved = *it;
        std::move(it + 1, blocks.end(), it);
        blocks.back() = saved;
        return true;
      }
    }
    return false;
  }

  void insert(uint64_t tag, bool isDirty = false) {
    blocks.push_back({tag, isDirty});
  }

  void evict(bool* wasDirty = nullptr) {
    Block victim = blocks.front();
    if (wasDirty) *wasDirty = victim.dirty;
    std::move(blocks.begin() + 1, blocks.end(), blocks.begin());
    blocks.pop_back();
  }

  void setDirty(uint64_t tag) {
    for (auto& b : blocks) {
      if (b.tag == tag) {
        b.dirty = true;
        return;
      }
    }
  }

  size_t size() const { return blocks.size(); }
};

struct Sim {
  CacheConfig config;
  CacheStats  stats;

  std::vector<LinearLRUSet> sets;
  std::unordered_set<uint64_t> seenTags;
  HashLRUSet faShadow;
  uint64_t faCapacity = 0;

  bool useBitwise = false;
  uint64_t lineShift = 0;
  uint64_t setMask = 0;
  uint64_t tagShift = 0;

  explicit Sim(const CacheConfig &cfg);

  [[nodiscard]] bool   access(uint64_t addr, bool isWrite, bool isPrefetch);
  [[nodiscard]] double hitRate() const;

  friend std::ostream& operator<<(std::ostream& os, const Sim& sim);
};

struct Tlb {
  TlbConfig config;
  TlbStats  stats;

  HashLRUSet store;

  explicit Tlb(const TlbConfig &cfg);

  [[nodiscard]] bool access(uint64_t addr);

  friend std::ostream& operator<<(std::ostream& os, const Tlb& tlb);
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

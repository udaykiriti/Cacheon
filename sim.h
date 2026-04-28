#ifndef CACHEON_SIM_H
#define CACHEON_SIM_H

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class WritePolicy
{
  WriteBack,
  WriteThrough
};

enum class Prefetcher
{
  None,
  NextLine,
  Stride
};

struct CacheConfig
{
  uint64_t size = 0;
  uint64_t lineSize = 0;
  uint64_t associativity = 0;
  bool useLRU = false;
  WritePolicy writePolicy = WritePolicy::WriteBack;
};

struct CacheStats
{
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

struct TlbConfig
{
  uint64_t entries = 0;
  uint64_t pageSize = 4096;
  bool useLRU = true;
};

struct TlbStats
{
  uint64_t hits = 0;
  uint64_t misses = 0;
};

struct Sim
{
  CacheConfig config;
  CacheStats stats;

  std::vector<std::unordered_map<uint64_t, bool>> sets;
  std::vector<std::deque<uint64_t>> setOrder;
  std::unordered_set<uint64_t> seenTags;
  std::unordered_set<uint64_t> faTags;
  std::deque<uint64_t> faOrder;
  uint64_t faCapacity = 0;

  explicit Sim(const CacheConfig &cfg);

  bool access(uint64_t addr, bool isWrite, bool isPrefetch);
  double hitRate() const;
  void reset();
};

struct Tlb
{
  TlbConfig config;
  TlbStats stats;
  std::unordered_set<uint64_t> tags;
  std::deque<uint64_t> order;

  explicit Tlb(const TlbConfig &cfg);

  bool access(uint64_t addr);
  void reset();
};

uint64_t passCount(uint64_t accessCount);
void accessAll(Sim &l1d, Sim &l2, Sim &l3, uint64_t addr, bool isWrite, bool isPrefetch);
void runSimulation(Sim &l1d, Sim &l2, Sim &l3, Tlb *tlb, uint64_t size, uint64_t stride,
                   bool randomAccess, uint64_t numPasses, uint64_t writeRate,
                   Prefetcher prefetcher, uint64_t &totalAccesses);

std::string makeBar(double percentage, int width = 20);
uint64_t parseSize(const char *str);
void printResults(const Sim &l1d, const Sim &l2, const Sim &l3, const Tlb *tlb,
                  uint64_t totalAccesses, bool sequential);

#endif

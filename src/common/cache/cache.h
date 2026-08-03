#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace atlas::cache {

// Counters every policy maintains, so LRU and LFU can be compared on the same workload.
// `hit_ratio` is the number the Phase 4 DoD cares about.
struct Stats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::size_t size = 0;
  std::size_t capacity = 0;

  double hit_ratio() const {
    const std::uint64_t total = hits + misses;
    return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
  }
};

// A fixed-capacity cache. The two implementations differ only in *which* entry they evict when
// full — that single decision is the whole subject of docs/concepts/lru-lfu-cache.md.
//
// Both are O(1) for Get and Put, including eviction: a cache that scanned to find its victim
// would spend more time choosing than the miss it is trying to avoid.
template <typename Key, typename Value>
class Cache {
 public:
  virtual ~Cache() = default;

  virtual std::optional<Value> Get(const Key& key) = 0;
  virtual void Put(const Key& key, Value value) = 0;
  virtual void Clear() = 0;

  virtual Stats stats() const = 0;
  virtual std::string policy() const = 0;
};

}  // namespace atlas::cache

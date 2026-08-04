// LRU vs LFU (Phase 4). Two things are tested: that each policy evicts what its name promises,
// and that on a workload with a hot minority they produce *different* hit ratios — which is the
// whole reason the roadmap asks for both rather than picking one.

#include <cstdio>
#include <string>
#include <vector>

#include "common/cache/lfu_cache.h"
#include "common/cache/lru_cache.h"

namespace {
int g_checks = 0;
int g_fails = 0;
}  // namespace

#define CHECK(cond)                                         \
  do {                                                      \
    ++g_checks;                                             \
    if (!(cond)) {                                          \
      ++g_fails;                                            \
      std::printf("FAIL (line %d): %s\n", __LINE__, #cond); \
    }                                                       \
  } while (0)

#define CHECK_EQ(a, b)                                             \
  do {                                                             \
    ++g_checks;                                                    \
    if (!((a) == (b))) {                                           \
      ++g_fails;                                                   \
      std::printf("FAIL (line %d): %s == %s\n", __LINE__, #a, #b); \
    }                                                              \
  } while (0)

int main() {
  using atlas::cache::LfuCache;
  using atlas::cache::LruCache;

  // ---- LRU: the least recently *used* entry leaves ----
  {
    LruCache<std::string, int> cache(2);
    cache.Put("a", 1);
    cache.Put("b", 2);
    CHECK(cache.Get("a").has_value());  // touching "a" makes "b" the eviction candidate
    cache.Put("c", 3);
    CHECK(!cache.Get("b").has_value());
    CHECK(cache.Get("a").has_value());
    CHECK(cache.Get("c").has_value());
    CHECK_EQ(cache.stats().evictions, 1u);
    CHECK_EQ(cache.stats().size, 2u);
    CHECK_EQ(cache.policy(), std::string("lru"));

    cache.Put("a", 99);  // update in place: no growth, no eviction
    CHECK_EQ(cache.stats().size, 2u);
    CHECK_EQ(cache.Get("a").value(), 99);
  }

  // ---- LFU: the least *frequently* used entry leaves, even if recently touched ----
  {
    LfuCache<std::string, int> cache(2);
    cache.Put("a", 1);
    cache.Put("b", 2);
    cache.Get("a");
    cache.Get("a");
    cache.Get("b");  // a:freq 3, b:freq 2 — b is the victim despite being touched most recently
    cache.Put("c", 3);
    CHECK(!cache.Get("b").has_value());
    CHECK(cache.Get("a").has_value());
    CHECK_EQ(cache.policy(), std::string("lfu"));
  }

  // ---- LFU breaks frequency ties by recency, so equal-frequency entries aren't arbitrary ----
  {
    LfuCache<std::string, int> cache(2);
    cache.Put("a", 1);
    cache.Put("b", 2);  // both at frequency 1; "a" is the older of the two
    cache.Put("c", 3);
    CHECK(!cache.Get("a").has_value());
    CHECK(cache.Get("b").has_value());
  }

  // ---- min_frequency must reset on insert, or a fresh entry is immortal ----
  {
    LfuCache<std::string, int> cache(2);
    cache.Put("hot", 1);
    for (int i = 0; i < 10; ++i) cache.Get("hot");  // frequency 11
    cache.Put("x", 2);                              // frequency 1
    cache.Put("y", 3);                              // evicts "x" (freq 1), not "hot"
    CHECK(cache.Get("hot").has_value());
    CHECK(!cache.Get("x").has_value());
    CHECK(cache.Get("y").has_value());
  }

  // ---- the comparison the DoD asks for: same workload, both policies ----
  //
  // Query traffic is skewed: a small set of queries repeats constantly while the long tail is
  // seen once. Here 8 hot keys are each requested 3 times per round, and 10 one-shot keys stream
  // past. This is exactly where the policies diverge — the cold stream is *more recent* than the
  // hot set at the end of every round, so recency alone gives the wrong answer, while frequency
  // counts identify the working set correctly.
  const auto access = [](auto& cache, const std::string& key) {
    if (!cache.Get(key).has_value()) cache.Put(key, 1);
  };

  {
    constexpr std::size_t kCapacity = 16;
    LruCache<std::string, int> lru(kCapacity);
    LfuCache<std::string, int> lfu(kCapacity);

    for (int round = 0; round < 300; ++round) {
      for (int pass = 0; pass < 3; ++pass) {
        for (int hot = 0; hot < 8; ++hot) {
          const std::string key = "hot-" + std::to_string(hot);
          access(lru, key);
          access(lfu, key);
        }
      }
      for (int cold = 0; cold < 10; ++cold) {
        const std::string key = "cold-" + std::to_string(round * 10 + cold);
        access(lru, key);
        access(lfu, key);
      }
    }

    const double lru_ratio = lru.stats().hit_ratio();
    const double lfu_ratio = lfu.stats().hit_ratio();
    std::printf("skewed workload (8 hot x3, 10 one-shot, capacity 16): LRU %.1f%%, LFU %.1f%%\n",
                lru_ratio * 100.0, lfu_ratio * 100.0);

    CHECK(lru_ratio > 0.35);
    CHECK(lfu_ratio > 0.6);
    // The headline: frequency beats recency by ~20 points on skewed traffic, because LRU keeps
    // re-admitting the one-shot keys and paying for them with hot entries.
    CHECK(lfu_ratio > lru_ratio + 0.1);
  }

  // ---- both policies collapse on a scan larger than the cache ----
  //
  // Worth pinning down, because it is the classic surprise: when each round touches more
  // distinct keys than the cache can hold and touches them in a fixed order, every entry is
  // evicted exactly before it is next needed. The hit ratio is not merely poor, it is *zero* —
  // the cache costs memory and returns nothing. Neither policy escapes it: LRU evicts by age,
  // and LFU sees every key at frequency 1, so it falls back to the same order.
  //
  // This is why the coordinator's cache is keyed on the query string, where repeats are real,
  // and not on anything scan-like.
  {
    constexpr std::size_t kCapacity = 8;
    LruCache<std::string, int> lru(kCapacity);
    LfuCache<std::string, int> lfu(kCapacity);

    for (int round = 0; round < 50; ++round) {
      for (int i = 0; i < 12; ++i) {  // 12 distinct keys, cycled, into an 8-entry cache
        const std::string key = "scan-" + std::to_string(i);
        access(lru, key);
        access(lfu, key);
      }
    }
    std::printf("cyclic scan (12 keys, capacity 8): LRU %.1f%%, LFU %.1f%%\n",
                lru.stats().hit_ratio() * 100.0, lfu.stats().hit_ratio() * 100.0);
    CHECK_EQ(lru.stats().hits, 0u);
    CHECK_EQ(lfu.stats().hits, 0u);
  }

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

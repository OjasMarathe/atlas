// Dependency-free tests for the consistent-hashing ring (piece #4).
// Runs standalone via clang++ AND as a CMake/CTest target — no gRPC or GoogleTest needed.

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/hash_ring.h"

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
  using namespace atlas;

  // Empty ring: no replicas, no owner.
  {
    HashRing r;
    CHECK(r.Replicas("k", 3).empty());
    CHECK(r.Owner("k").empty());
  }

  // Replicas: 3 distinct physical nodes, deterministic, primary == Owner.
  {
    HashRing r(128);
    for (const char* n : {"node1", "node2", "node3", "node4"}) r.AddNode(n);
    CHECK_EQ(r.NodeCount(), size_t(4));
    auto reps = r.Replicas("some-chunk-id", 3);
    CHECK_EQ(reps.size(), size_t(3));
    CHECK_EQ(std::set<std::string>(reps.begin(), reps.end()).size(), size_t(3));  // distinct
    CHECK(reps == r.Replicas("some-chunk-id", 3));                                // deterministic
    CHECK_EQ(r.Owner("some-chunk-id"), reps[0]);
  }

  // Load balance: 4 nodes should each own roughly a quarter of the keys.
  {
    HashRing r(200);
    const std::vector<std::string> nodes = {"n1", "n2", "n3", "n4"};
    for (const auto& n : nodes) r.AddNode(n);
    std::map<std::string, int> counts;
    const int K = 12000;
    for (int i = 0; i < K; ++i) counts[r.Owner("key-" + std::to_string(i))]++;
    for (const auto& n : nodes) {
      const double frac = counts[n] / double(K);
      CHECK(frac > 0.15 && frac < 0.35);  // ideal 0.25, generous tolerance
    }
  }

  // The consistent-hashing property: removing a node moves ONLY its own keys, not everyone's.
  {
    HashRing r(200);
    for (const char* n : {"n1", "n2", "n3", "n4"}) r.AddNode(n);
    const int K = 12000;
    std::vector<std::string> keys;
    std::vector<std::string> before;
    for (int i = 0; i < K; ++i) {
      keys.push_back("k-" + std::to_string(i));
      before.push_back(r.Owner(keys.back()));
    }
    int owned_by_n3 = 0;
    for (const auto& b : before)
      if (b == "n3") ++owned_by_n3;

    r.RemoveNode("n3");

    int moved = 0;
    for (int i = 0; i < K; ++i)
      if (r.Owner(keys[i]) != before[i]) ++moved;

    CHECK(moved / double(K) < 0.35);  // NOT ~1.0, which is what `hash % N` would give
    CHECK_EQ(moved, owned_by_n3);     // exactly the removed node's keys moved; all others stayed
  }

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

#ifndef ATLAS_COMMON_HASH_RING_H_
#define ATLAS_COMMON_HASH_RING_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace atlas {

using NodeId = std::string;

// Consistent-hashing ring with virtual nodes. Decides which nodes hold a chunk so that a node
// join/leave migrates only ~1/N of the data. See docs/concepts/consistent-hashing.md.
class HashRing {
 public:
  explicit HashRing(int vnodes_per_node = 128) : vnodes_(vnodes_per_node) {}

  void AddNode(const NodeId& node);
  void RemoveNode(const NodeId& node);

  bool Empty() const { return ring_.empty(); }
  size_t NodeCount() const;  // number of distinct physical nodes

  // The R distinct physical nodes responsible for `key`, walking clockwise from its hash position:
  // [primary, secondary, tertiary, ...]. Returns fewer than R only if fewer nodes exist.
  std::vector<NodeId> Replicas(std::string_view key, int r) const;

  // Convenience: the primary owner (Replicas(key, 1)[0], or "" if the ring is empty).
  NodeId Owner(std::string_view key) const;

 private:
  int vnodes_;
  std::map<uint64_t, NodeId> ring_;  // ring position -> physical node
};

}  // namespace atlas

#endif  // ATLAS_COMMON_HASH_RING_H_

#include "common/hash_ring.h"

#include <string>
#include <unordered_set>

#include "common/sha256.h"

namespace atlas {
namespace {

// Map a key onto the ring: the first 8 bytes (16 hex chars) of its SHA-256, big-endian.
uint64_t Hash64(std::string_view s) {
  const std::string hex = Sha256Hex(s.data(), s.size());
  uint64_t v = 0;
  for (int i = 0; i < 16; ++i) {
    const char c = hex[i];
    const uint64_t d = (c >= '0' && c <= '9') ? uint64_t(c - '0') : uint64_t(c - 'a' + 10);
    v = (v << 4) | d;
  }
  return v;
}

std::string VnodeKey(const NodeId& node, int i) { return node + "#" + std::to_string(i); }

}  // namespace

void HashRing::AddNode(const NodeId& node) {
  for (int i = 0; i < vnodes_; ++i) ring_[Hash64(VnodeKey(node, i))] = node;
}

void HashRing::RemoveNode(const NodeId& node) {
  for (int i = 0; i < vnodes_; ++i) ring_.erase(Hash64(VnodeKey(node, i)));
}

size_t HashRing::NodeCount() const {
  std::unordered_set<NodeId> nodes;
  for (const auto& [pos, n] : ring_) nodes.insert(n);
  return nodes.size();
}

std::vector<NodeId> HashRing::Replicas(std::string_view key, int r) const {
  std::vector<NodeId> out;
  if (ring_.empty() || r <= 0) return out;
  std::unordered_set<NodeId> seen;
  auto it = ring_.lower_bound(Hash64(key));  // first vnode clockwise at/after the key
  for (size_t steps = 0; steps < ring_.size() && static_cast<int>(out.size()) < r; ++steps) {
    if (it == ring_.end()) it = ring_.begin();  // wrap around the ring
    const NodeId& n = it->second;
    if (seen.insert(n).second) out.push_back(n);  // skip vnodes of already-chosen physical nodes
    ++it;
  }
  return out;
}

NodeId HashRing::Owner(std::string_view key) const {
  const auto r = Replicas(key, 1);
  return r.empty() ? NodeId{} : r[0];
}

}  // namespace atlas

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace atlas {

// The cluster's liveness view, driven by probe results.
//
// A node is declared dead only after `failure_threshold` *consecutive* failed probes — one lost
// packet or a GC pause must not evict a healthy node — and a single success revives it
// immediately. See docs/concepts/heartbeat-failure-detection.md.
class HealthTracker {
 public:
  explicit HealthTracker(int failure_threshold = 3) : threshold_(failure_threshold) {}

  void RecordSuccess(const std::string& node_id);
  void RecordFailure(const std::string& node_id);
  void Forget(const std::string& node_id);  // node left the cluster entirely

  // Drop every tracked node that is no longer a cluster member. Without this a node that leaves
  // while it happened to be failing stays in the map at its last failure count forever — nothing
  // probes it any more, so nothing can ever reset it, and DeadNodes() reports a node that simply
  // left as permanently down.
  void Retain(const std::vector<std::string>& members);

  // A node with fewer than `threshold` consecutive failures is alive. A node never probed is
  // considered alive: it just joined, and treating the unprobed as dead would declare a whole
  // fresh cluster dead at startup.
  bool IsAlive(const std::string& node_id) const;

  int ConsecutiveFailures(const std::string& node_id) const;
  std::vector<std::string> DeadNodes() const;  // sorted, so output is deterministic
  int failure_threshold() const { return threshold_; }

 private:
  int threshold_;
  std::unordered_map<std::string, int> failures_;  // node_id -> consecutive failed probes
};

}  // namespace atlas

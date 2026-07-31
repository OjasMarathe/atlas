#include "cluster/health_tracker.h"

#include <algorithm>

namespace atlas {

void HealthTracker::RecordSuccess(const std::string& node_id) { failures_[node_id] = 0; }

void HealthTracker::RecordFailure(const std::string& node_id) { ++failures_[node_id]; }

void HealthTracker::Forget(const std::string& node_id) { failures_.erase(node_id); }

bool HealthTracker::IsAlive(const std::string& node_id) const {
  const auto it = failures_.find(node_id);
  if (it == failures_.end()) return true;  // never probed -> assume alive
  return it->second < threshold_;
}

int HealthTracker::ConsecutiveFailures(const std::string& node_id) const {
  const auto it = failures_.find(node_id);
  return it == failures_.end() ? 0 : it->second;
}

std::vector<std::string> HealthTracker::DeadNodes() const {
  std::vector<std::string> dead;
  for (const auto& [node_id, count] : failures_) {
    if (count >= threshold_) dead.push_back(node_id);
  }
  std::sort(dead.begin(), dead.end());
  return dead;
}

}  // namespace atlas

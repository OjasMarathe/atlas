#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "cluster/healer.h"
#include "cluster/health_tracker.h"
#include "cluster/prober.h"
#include "metadata/metadata_store.h"

namespace atlas {

struct MaintenanceOptions {
  std::chrono::milliseconds interval{2000};       // between probe+heal rounds
  std::chrono::milliseconds probe_timeout{1000};  // per-node heartbeat deadline
  int failure_threshold = 3;                      // consecutive misses before a node is dead
  int replication_factor = 3;
};

// The control plane's background loop: probe every member for liveness, then repair any chunk
// that has fallen below the replication factor. This is what makes healing *automatic* — without
// it, detection and repair only happen when something calls them explicitly.
//
// Runs on the metadata node (ADR-0009: the authoritative control plane owns liveness).
class ClusterMaintenance {
 public:
  using RingSnapshot = std::function<RingState()>;
  using LogFn = std::function<void(const std::string&)>;

  ClusterMaintenance(MetadataStore* store, RingSnapshot snapshot, LogFn log,
                     MaintenanceOptions options = {});
  ~ClusterMaintenance();

  ClusterMaintenance(const ClusterMaintenance&) = delete;
  ClusterMaintenance& operator=(const ClusterMaintenance&) = delete;

  void Start();
  void Stop();

  // One probe+heal round. Public and synchronous so tests (and a future admin RPC) can drive a
  // round directly instead of waiting on the timer.
  HealReport RunOnce();

  const HealthTracker& tracker() const { return tracker_; }

 private:
  void Loop();

  MaintenanceOptions options_;
  HealthTracker tracker_;
  Prober prober_;
  Healer healer_;
  RingSnapshot snapshot_;
  LogFn log_;
  std::vector<std::string> last_dead_;  // to log only liveness *transitions*
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace atlas

#include "cluster/maintenance.h"

#include <utility>

namespace atlas {
namespace {

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const std::string& item : items) {
    if (!out.empty()) out += ", ";
    out += item;
  }
  return out;
}

}  // namespace

ClusterMaintenance::ClusterMaintenance(MetadataStore* store, RingSnapshot snapshot, LogFn log,
                                       MaintenanceOptions options)
    : options_(options),
      tracker_(options.failure_threshold),
      prober_(&tracker_, options.probe_timeout),
      healer_(store, &tracker_, options.replication_factor),
      snapshot_(std::move(snapshot)),
      log_(std::move(log)) {}

ClusterMaintenance::~ClusterMaintenance() { Stop(); }

MaintenanceReport ClusterMaintenance::RunOnce() {
  const RingState ring = snapshot_();

  std::vector<ProbeTarget> targets;
  std::vector<std::string> members;
  targets.reserve(ring.nodes_size());
  members.reserve(ring.nodes_size());
  for (const NodeInfo& node : ring.nodes()) {
    targets.push_back(ProbeTarget{node.node_id(), node.address()});
    members.push_back(node.node_id());
  }
  // Drop liveness state for nodes that have left: they are no longer probed, so a failure count
  // recorded just before they left would never be cleared and they'd read as dead forever.
  tracker_.Retain(members);
  prober_.ProbeOnce(targets);

  // Report liveness *changes* only — logging the same dead node every round is noise.
  std::vector<std::string> dead = tracker_.DeadNodes();
  if (dead != last_dead_ && log_) {
    if (dead.empty()) {
      log_("all nodes healthy");
    } else {
      log_("nodes down: " + Join(dead));
    }
    last_dead_ = dead;
  }

  MaintenanceReport report;
  report.heal = healer_.RepairOnce(ring);
  if (log_ && (report.heal.repaired > 0 || report.heal.unrepairable > 0)) {
    log_("healed " + std::to_string(report.heal.repaired) + " replica(s) across " +
         std::to_string(report.heal.under_replicated) + " under-replicated chunk(s)" +
         (report.heal.unrepairable > 0
              ? "; " + std::to_string(report.heal.unrepairable) + " unrepairable (no live copy)"
              : ""));
  }

  // Repair first, then rebalance: a chunk that is short a copy must be made whole before anyone
  // worries about whether it is on the *right* nodes.
  if (options_.rebalance) {
    report.rebalance = healer_.RebalanceOnce(ring);
    if (log_ && report.rebalance.migrated > 0) {
      log_("migrated " + std::to_string(report.rebalance.migrated) + " replica(s) to their ring " +
           "position across " + std::to_string(report.rebalance.misplaced) + " chunk(s); evicted " +
           std::to_string(report.rebalance.evicted) + " stale copy(ies)");
    }
  }
  return report;
}

void ClusterMaintenance::Loop() {
  while (running_.load()) {
    RunOnce();
    // Sleep in slices so Stop() doesn't wait out a whole interval.
    for (int slept = 0; running_.load() && slept < options_.interval.count(); slept += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void ClusterMaintenance::Start() {
  if (running_.exchange(true)) return;  // already running
  thread_ = std::thread([this] { Loop(); });
}

void ClusterMaintenance::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

}  // namespace atlas

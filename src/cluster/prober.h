#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "cluster/health_tracker.h"
#include "storage.grpc.pb.h"

namespace atlas {

struct ProbeTarget {
  std::string node_id;
  std::string address;
};

// Pull-based failure detection: the control plane probes every storage node with the existing
// StorageService.Heartbeat RPC and feeds each result into a HealthTracker.
//
// Pull (rather than nodes pushing heartbeats) keeps liveness in exactly one place — the metadata
// node is already the authoritative control plane (ADR-0005) — and needs no new RPC. It costs
// O(nodes) probes per round, which is fine at M1 scale; a push/gossip design is the upgrade path.
class Prober {
 public:
  explicit Prober(HealthTracker* tracker,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
      : tracker_(tracker), timeout_(timeout) {}

  // One synchronous round over `targets`; returns how many answered. Synchronous and side-effect
  // complete on return, so tests drive detection deterministically instead of sleeping.
  int ProbeOnce(const std::vector<ProbeTarget>& targets);

 private:
  StorageService::Stub* StubFor(const std::string& address);

  HealthTracker* tracker_;
  std::chrono::milliseconds timeout_;
  std::unordered_map<std::string, std::unique_ptr<StorageService::Stub>> stubs_;
};

}  // namespace atlas

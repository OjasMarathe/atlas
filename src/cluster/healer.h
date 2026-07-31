#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "cluster/health_tracker.h"
#include "metadata/metadata_store.h"
#include "storage.grpc.pb.h"

namespace atlas {

struct HealReport {
  int chunks_scanned = 0;
  int under_replicated = 0;  // chunks with fewer than R live holders
  int repaired = 0;          // new replicas successfully created
  int unrepairable = 0;      // no live holder could serve the bytes
};

// Self-healing re-replication (Phase 2).
//
// Restores the replication factor after a node dies: for every chunk whose live holders have
// fallen below R, copy the bytes from a survivor onto a fresh ring-chosen node and record the new
// holder. This is what turns "the read worked because we routed around the dead node" into "the
// cluster is genuinely back to R copies".
//
// Bytes currently flow through the healer (pull from a survivor, push to the target). A
// source-driven push RPC would keep them off the control plane — a documented upgrade.
class Healer {
 public:
  Healer(MetadataStore* store, const HealthTracker* tracker, int replication_factor = 3)
      : store_(store), tracker_(tracker), replication_factor_(replication_factor) {}

  // One synchronous pass over every known chunk, using `ring_state` for membership + addresses.
  // Synchronous by design: a background loop can call it on a timer, and tests can call it
  // directly instead of sleeping and hoping.
  HealReport RepairOnce(const RingState& ring_state);

 private:
  StorageService::Stub* StubFor(const std::string& address);

  MetadataStore* store_;
  const HealthTracker* tracker_;
  int replication_factor_;
  std::unordered_map<std::string, std::unique_ptr<StorageService::Stub>> stubs_;
};

}  // namespace atlas

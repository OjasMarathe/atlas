#include "cluster/healer.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/hash_ring.h"
#include "storage/chunk_transfer.h"

namespace atlas {

StorageService::Stub* Healer::StubFor(const std::string& address) {
  auto it = stubs_.find(address);
  if (it == stubs_.end()) {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    it = stubs_.emplace(address, StorageService::NewStub(channel)).first;
  }
  return it->second.get();
}

HealReport Healer::RepairOnce(const RingState& ring_state) {
  HealReport report;

  HashRing ring(static_cast<int>(ring_state.virtual_nodes_per_node()));
  std::unordered_map<std::string, std::string> address;
  for (const NodeInfo& node : ring_state.nodes()) {
    ring.AddNode(node.node_id());
    address[node.node_id()] = node.address();
  }
  if (ring.Empty()) return report;

  for (const std::string& chunk_id : store_->AllChunkIds()) {
    ++report.chunks_scanned;

    const std::vector<std::string> holders = store_->ChunkLocations(chunk_id);
    std::unordered_set<std::string> holder_set(holders.begin(), holders.end());

    std::vector<std::string> live;
    for (const std::string& holder : holders) {
      if (tracker_->IsAlive(holder) && address.count(holder) != 0) live.push_back(holder);
    }
    if (static_cast<int>(live.size()) >= replication_factor_) continue;  // healthy

    ++report.under_replicated;

    // Pull the bytes from any survivor. Content addressing means every replica is equally
    // authoritative — there is no primary to consult.
    std::string data;
    bool have_data = false;
    for (const std::string& holder : live) {
      if (GetChunk(StubFor(address[holder]), chunk_id, &data)) {
        have_data = true;
        break;
      }
    }
    if (!have_data) {
      ++report.unrepairable;  // every copy is gone or unreachable — nothing to heal from
      continue;
    }

    // Walk the ring in preference order for live nodes that don't already hold this chunk.
    int needed = replication_factor_ - static_cast<int>(live.size());
    for (const NodeId& candidate : ring.Replicas(chunk_id, static_cast<int>(ring.NodeCount()))) {
      if (needed == 0) break;
      if (holder_set.count(candidate) != 0) continue;  // already a holder (live or dead)
      if (!tracker_->IsAlive(candidate)) continue;     // don't heal onto a dead node
      if (address.count(candidate) == 0) continue;     // not in the current membership
      if (ReplicateChunk(StubFor(address[candidate]), chunk_id, data)) {
        store_->AddChunkLocation(chunk_id, candidate);
        holder_set.insert(candidate);
        ++report.repaired;
        --needed;
      }
    }
  }
  return report;
}

RebalanceReport Healer::RebalanceOnce(const RingState& ring_state) {
  RebalanceReport report;

  HashRing ring(static_cast<int>(ring_state.virtual_nodes_per_node()));
  std::unordered_map<std::string, std::string> address;
  for (const NodeInfo& node : ring_state.nodes()) {
    ring.AddNode(node.node_id());
    address[node.node_id()] = node.address();
  }
  if (ring.Empty()) return report;

  for (const std::string& chunk_id : store_->AllChunkIds()) {
    ++report.chunks_scanned;

    const std::vector<std::string> holders = store_->ChunkLocations(chunk_id);
    std::vector<std::string> live;
    for (const std::string& holder : holders) {
      if (tracker_->IsAlive(holder) && address.count(holder) != 0) live.push_back(holder);
    }
    // Durability first: a degraded chunk is RepairOnce's problem, and moving replicas around
    // while a copy is already missing is exactly when we can least afford a mistake.
    if (static_cast<int>(live.size()) < replication_factor_) {
      ++report.skipped_degraded;
      continue;
    }

    const std::vector<NodeId> ideal = ring.Replicas(chunk_id, replication_factor_);
    const std::unordered_set<std::string> ideal_set(ideal.begin(), ideal.end());
    const std::unordered_set<std::string> live_set(live.begin(), live.end());

    // Where the ring wants this chunk but it isn't.
    std::vector<std::string> wanted;
    for (const NodeId& node : ideal) {
      if (live_set.count(node) == 0 && tracker_->IsAlive(node)) wanted.push_back(node);
    }
    if (wanted.empty()) continue;  // already sits where it belongs
    ++report.misplaced;

    std::string data;
    bool have_data = false;
    for (const std::string& holder : live) {
      if (GetChunk(StubFor(address[holder]), chunk_id, &data)) {
        have_data = true;
        break;
      }
    }
    if (!have_data) continue;  // can't read it; RepairOnce will surface this as unrepairable

    // Replicas that the ring no longer wants here — eviction candidates, consumed one per
    // successful migration so the holder count lands back at exactly R.
    std::vector<std::string> surplus;
    for (const std::string& holder : live) {
      if (ideal_set.count(holder) == 0) surplus.push_back(holder);
    }

    for (const std::string& target : wanted) {
      if (!ReplicateChunk(StubFor(address[target]), chunk_id, data)) continue;
      store_->AddChunkLocation(chunk_id, target);
      ++report.migrated;

      // Only now that the new copy is durable may an old one go.
      if (!surplus.empty()) {
        const std::string& stale = surplus.back();
        if (DeleteChunk(StubFor(address[stale]), chunk_id)) {
          store_->RemoveChunkLocation(chunk_id, stale);
          ++report.evicted;
        }
        surplus.pop_back();
      }
    }
  }
  return report;
}

}  // namespace atlas

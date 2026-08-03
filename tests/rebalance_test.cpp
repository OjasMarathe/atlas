// Node-join migration (the Phase 1 DoD's last clause).
//
// Fills a 5-node cluster with many chunks, adds a 6th node, and rebalances. Asserts the property
// that makes consistent hashing worth implementing: the new node takes over *its share* and
// almost nothing else moves — with `hash % N` placement, essentially every replica would relocate.
// Also checks the invariants: every chunk keeps exactly R live holders throughout, and the file
// still downloads byte-for-byte after all that data movement.

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "client/client.h"
#include "cluster/healer.h"
#include "cluster/health_tracker.h"
#include "metadata.grpc.pb.h"
#include "metadata/metadata_service.h"
#include "metadata/metadata_store.h"
#include "storage/chunk_store.h"
#include "storage/storage_service.h"

namespace {
int g_checks = 0;
int g_fails = 0;
namespace fs = std::filesystem;
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

namespace {

constexpr int kReplication = 3;
constexpr std::size_t kTestChunkSize = 64;  // small, so one small file yields many chunks
constexpr int kBlocks = 40;

struct StorageNode {
  std::string node_id;
  std::string address;
  std::string path;
  std::unique_ptr<atlas::ChunkStore> store;
  std::unique_ptr<atlas::StorageServiceImpl> service;
  std::unique_ptr<grpc::Server> server;
};

std::unique_ptr<StorageNode> StartStorageNode(const std::string& id) {
  auto node = std::make_unique<StorageNode>();
  node->node_id = id;
  node->path = (fs::temp_directory_path() / ("atlas_rebal_" + id)).string();
  fs::remove_all(node->path);
  node->store = std::make_unique<atlas::ChunkStore>(node->path);
  node->service = std::make_unique<atlas::StorageServiceImpl>(node->store.get());
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(node->service.get());
  node->server = builder.BuildAndStart();
  node->address = "127.0.0.1:" + std::to_string(port);
  return node;
}

// Distinct 64-byte blocks: identical blocks would dedup to one content-addressed chunk and we'd
// have far fewer placements to measure than intended.
std::string MakePayload() {
  std::string payload;
  for (int i = 0; i < kBlocks; ++i) {
    std::string block = "block-" + std::to_string(i) + "-";
    block.resize(kTestChunkSize, static_cast<char>('a' + (i % 26)));
    payload += block;
  }
  return payload;
}

// chunk_id -> its holders, as the metadata store currently sees it.
std::map<std::string, std::set<std::string>> Placement(const atlas::MetadataStore& store) {
  std::map<std::string, std::set<std::string>> placement;
  for (const std::string& chunk_id : store.AllChunkIds()) {
    const std::vector<std::string> holders = store.ChunkLocations(chunk_id);
    placement[chunk_id] = std::set<std::string>(holders.begin(), holders.end());
  }
  return placement;
}

}  // namespace

int main() {
  using namespace atlas;

  const std::string meta_path = (fs::temp_directory_path() / "atlas_rebal_meta").string();
  fs::remove_all(meta_path);
  MetadataStore meta_store(meta_path);
  CHECK(meta_store.ok());
  MetadataServiceImpl meta_service(&meta_store, /*vnodes_per_node=*/128);

  int meta_port = 0;
  grpc::ServerBuilder meta_builder;
  meta_builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &meta_port);
  meta_builder.RegisterService(&meta_service);
  std::unique_ptr<grpc::Server> meta_server = meta_builder.BuildAndStart();
  const std::string meta_address = "127.0.0.1:" + std::to_string(meta_port);
  auto meta_stub = MetadataService::NewStub(
      grpc::CreateChannel(meta_address, grpc::InsecureChannelCredentials()));

  const auto join = [&](const std::string& node_id, const std::string& address) {
    grpc::ClientContext ctx;
    UpdateMembershipRequest req;
    RingState resp;
    req.mutable_node()->set_node_id(node_id);
    req.mutable_node()->set_address(address);
    req.set_change(UpdateMembershipRequest::JOIN);
    CHECK(meta_stub->UpdateMembership(&ctx, req, &resp).ok());
  };
  const auto ring_state = [&] {
    grpc::ClientContext ctx;
    GetRingRequest req;
    RingState state;
    meta_stub->GetRing(&ctx, req, &state);
    return state;
  };

  // ---- a 5-node cluster, filled with chunks ----
  std::vector<std::unique_ptr<StorageNode>> nodes;
  for (int i = 1; i <= 5; ++i) {
    nodes.push_back(StartStorageNode("node" + std::to_string(i)));
    join(nodes.back()->node_id, nodes.back()->address);
  }

  const std::string payload = MakePayload();
  AtlasClient client(meta_address, kTestChunkSize);
  CHECK(client.Upload("big.bin", "ojas", payload).ok);

  const std::map<std::string, std::set<std::string>> before = Placement(meta_store);
  CHECK_EQ(before.size(), static_cast<std::size_t>(kBlocks));  // all blocks distinct
  int replicas_before = 0;
  for (const auto& [chunk_id, holders] : before) {
    CHECK_EQ(holders.size(), static_cast<std::size_t>(kReplication));
    replicas_before += static_cast<int>(holders.size());
  }

  HealthTracker tracker(/*failure_threshold=*/2);
  Healer healer(&meta_store, &tracker, kReplication);

  // Nothing to do while membership is unchanged.
  {
    const RebalanceReport report = healer.RebalanceOnce(ring_state());
    CHECK_EQ(report.misplaced, 0);
    CHECK_EQ(report.migrated, 0);
  }

  // ---- a sixth node joins ----
  nodes.push_back(StartStorageNode("node6"));
  join(nodes.back()->node_id, nodes.back()->address);

  int total_migrated = 0;
  for (int round = 0; round < 5; ++round) {
    const RebalanceReport report = healer.RebalanceOnce(ring_state());
    total_migrated += report.migrated;
    if (report.migrated == 0) break;  // converged
  }
  // Converged: another pass finds nothing misplaced.
  CHECK_EQ(healer.RebalanceOnce(ring_state()).migrated, 0);

  const std::map<std::string, std::set<std::string>> after = Placement(meta_store);

  // Invariant: replication factor preserved for every chunk, nothing lost or duplicated.
  CHECK_EQ(after.size(), before.size());
  int replicas_after = 0;
  for (const auto& [chunk_id, holders] : after) {
    CHECK_EQ(holders.size(), static_cast<std::size_t>(kReplication));
    replicas_after += static_cast<int>(holders.size());
  }
  CHECK_EQ(replicas_after, replicas_before);

  // The new node took over a real share of the data.
  int node6_replicas = 0;
  for (const auto& [chunk_id, holders] : after) {
    if (holders.count("node6") != 0) ++node6_replicas;
  }
  CHECK(node6_replicas > 0);

  // THE PROPERTY: only ~1/6 of replicas moved. `hash % N` placement would have relocated
  // essentially all of them.
  int moved = 0;
  for (const auto& [chunk_id, holders] : after) {
    const auto it = before.find(chunk_id);
    if (it == before.end()) continue;
    for (const std::string& holder : holders) {
      if (it->second.count(holder) == 0) ++moved;  // this replica is on a node it wasn't on
    }
  }
  const double moved_fraction = static_cast<double>(moved) / replicas_before;
  std::printf("rebalance: %d/%d replicas moved (%.1f%%), node6 now holds %d/%d chunks\n", moved,
              replicas_before, moved_fraction * 100.0, node6_replicas, kBlocks);
  CHECK(moved_fraction < 0.35);     // ideal is ~1/6 = 17%; nowhere near "everything"
  CHECK_EQ(moved, node6_replicas);  // every move landed on the newcomer, nothing churned
  CHECK_EQ(total_migrated, node6_replicas);

  // ---- and the data survived the move ----
  const DownloadResult down = client.Download("big.bin");
  CHECK(down.ok);
  CHECK_EQ(down.data, payload);

  for (auto& node : nodes) {
    if (node->server) node->server->Shutdown();
  }
  meta_server->Shutdown();
  fs::remove_all(meta_path);
  for (const auto& node : nodes) fs::remove_all(node->path);

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

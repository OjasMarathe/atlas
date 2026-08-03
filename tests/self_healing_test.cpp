// Phase 2 headline test: the cluster repairs itself.
//
// Upload a file (3 replicas) -> kill a holder -> failure detection marks it dead -> the healer
// copies the chunk onto a fresh live node -> the chunk is back to 3 live holders and the file
// still downloads. Everything is driven synchronously, so the test never sleeps on a timer.

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "client/client.h"
#include "cluster/healer.h"
#include "cluster/health_tracker.h"
#include "cluster/prober.h"
#include "metadata.grpc.pb.h"
#include "metadata/metadata_service.h"
#include "metadata/metadata_store.h"
#include "storage/chunk_store.h"
#include "storage/chunk_transfer.h"
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

struct StorageNode {
  std::string node_id;
  std::string address;
  std::string path;
  std::unique_ptr<atlas::ChunkStore> store;
  std::unique_ptr<atlas::StorageServiceImpl> service;
  std::unique_ptr<grpc::Server> server;

  void Kill() {
    if (server) {
      server->Shutdown();
      server.reset();
    }
  }
};

std::unique_ptr<StorageNode> StartStorageNode(const std::string& id) {
  auto node = std::make_unique<StorageNode>();
  node->node_id = id;
  node->path = (fs::temp_directory_path() / ("atlas_heal_" + id)).string();
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

}  // namespace

int main() {
  using namespace atlas;

  // ---- control plane ----
  const std::string meta_path = (fs::temp_directory_path() / "atlas_heal_meta").string();
  fs::remove_all(meta_path);
  MetadataStore meta_store(meta_path);
  CHECK(meta_store.ok());
  MetadataServiceImpl meta_service(&meta_store, /*vnodes_per_node=*/64);
  int meta_port = 0;
  grpc::ServerBuilder meta_builder;
  meta_builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &meta_port);
  meta_builder.RegisterService(&meta_service);
  std::unique_ptr<grpc::Server> meta_server = meta_builder.BuildAndStart();
  const std::string meta_address = "127.0.0.1:" + std::to_string(meta_port);
  auto meta_stub = MetadataService::NewStub(
      grpc::CreateChannel(meta_address, grpc::InsecureChannelCredentials()));

  // ---- 4 storage nodes, all joined ----
  std::vector<std::unique_ptr<StorageNode>> nodes;
  for (int i = 1; i <= 4; ++i) nodes.push_back(StartStorageNode("storage-" + std::to_string(i)));
  for (const auto& node : nodes) {
    grpc::ClientContext ctx;
    UpdateMembershipRequest req;
    RingState resp;
    req.mutable_node()->set_node_id(node->node_id);
    req.mutable_node()->set_address(node->address);
    req.set_change(UpdateMembershipRequest::JOIN);
    CHECK(meta_stub->UpdateMembership(&ctx, req, &resp).ok());
  }
  const auto ring_state = [&] {
    grpc::ClientContext ctx;
    GetRingRequest req;
    RingState state;
    meta_stub->GetRing(&ctx, req, &state);
    return state;
  };
  const auto address_of = [&](const std::string& id) {
    for (const auto& node : nodes) {
      if (node->node_id == id) return node->address;
    }
    return std::string{};
  };

  // ---- upload: 3 holders ----
  AtlasClient client(meta_address);
  const std::string content = "phase two: the cluster heals itself. " + std::string(500, 'z');
  CHECK(client.Upload("heal.txt", "ojas", content).ok);

  FileMetadata meta;
  {
    grpc::ClientContext ctx;
    GetFileRequest req;
    req.set_file_id("heal.txt");
    CHECK(meta_stub->GetFile(&ctx, req, &meta).ok());
  }
  CHECK_EQ(meta.chunks_size(), 1);
  const std::string chunk_id = meta.chunks(0).chunk().chunk_id();
  CHECK_EQ(meta.chunks(0).node_ids_size(), 3);
  const std::vector<std::string> original(meta.chunks(0).node_ids().begin(),
                                          meta.chunks(0).node_ids().end());

  HealthTracker tracker(/*failure_threshold=*/2);
  Prober prober(&tracker, std::chrono::milliseconds(500));
  Healer healer(&meta_store, &tracker, /*replication_factor=*/3);

  std::vector<ProbeTarget> targets;
  for (const auto& node : nodes) targets.push_back({node->node_id, node->address});

  // A healthy cluster needs no repair.
  prober.ProbeOnce(targets);
  {
    const HealReport report = healer.RepairOnce(ring_state());
    CHECK(report.chunks_scanned > 0);
    CHECK_EQ(report.under_replicated, 0);
    CHECK_EQ(report.repaired, 0);
  }

  // ---- kill one of the chunk's holders ----
  const std::string dead = original[1];
  for (const auto& node : nodes) {
    if (node->node_id == dead) node->Kill();
  }

  // Failure detection: threshold is 2, so it takes two missed probes.
  prober.ProbeOnce(targets);
  CHECK(tracker.IsAlive(dead));
  prober.ProbeOnce(targets);
  CHECK(!tracker.IsAlive(dead));

  // ---- heal ----
  const HealReport report = healer.RepairOnce(ring_state());
  CHECK(report.under_replicated >= 1);
  CHECK_EQ(report.repaired, 1);  // exactly one new replica restores R=3
  CHECK_EQ(report.unrepairable, 0);

  // The chunk now has a holder that is NOT one of the originals, and it really has the bytes.
  const std::vector<std::string> holders = meta_store.ChunkLocations(chunk_id);
  std::vector<std::string> fresh;
  for (const std::string& holder : holders) {
    if (std::find(original.begin(), original.end(), holder) == original.end()) {
      fresh.push_back(holder);
    }
  }
  CHECK_EQ(fresh.size(), 1u);
  if (fresh.size() == 1) {
    std::printf("healed: chunk re-replicated onto %s after %s died\n", fresh[0].c_str(),
                dead.c_str());
    auto stub = StorageService::NewStub(
        grpc::CreateChannel(address_of(fresh[0]), grpc::InsecureChannelCredentials()));
    std::string bytes;
    CHECK(GetChunk(stub.get(), chunk_id, &bytes));  // the new replica serves the real chunk
    CHECK_EQ(bytes, content);
  }

  // Live holders are back to the replication factor.
  int live_holders = 0;
  for (const std::string& holder : holders) {
    if (tracker.IsAlive(holder)) ++live_holders;
  }
  CHECK_EQ(live_holders, 3);

  // Healing converges: a second pass finds nothing left to do.
  {
    const HealReport again = healer.RepairOnce(ring_state());
    CHECK_EQ(again.under_replicated, 0);
    CHECK_EQ(again.repaired, 0);
  }

  // And the file still downloads.
  const DownloadResult down = client.Download("heal.txt");
  CHECK(down.ok);
  CHECK_EQ(down.data, content);

  for (auto& node : nodes) node->Kill();
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

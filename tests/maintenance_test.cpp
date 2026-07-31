// Phase 2 wiring test: ClusterMaintenance does probe-then-heal in one round.
//
// self_healing_test drives the Prober and Healer by hand; this covers the loop that runs them
// together on the metadata node — the piece the live demo exercises but CI otherwise wouldn't.

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "client/client.h"
#include "cluster/maintenance.h"
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
  node->path = (fs::temp_directory_path() / ("atlas_maint_" + id)).string();
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

  const std::string meta_path = (fs::temp_directory_path() / "atlas_maint_meta").string();
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

  // SnapshotRing() is what the loop uses to see membership without touching guarded state.
  CHECK_EQ(meta_service.SnapshotRing().nodes_size(), 4);

  AtlasClient client(meta_address);
  const std::string content = "maintenance loop keeps the cluster whole";
  CHECK(client.Upload("m.txt", "ojas", content).ok);

  FileMetadata meta;
  {
    grpc::ClientContext ctx;
    GetFileRequest req;
    req.set_file_id("m.txt");
    CHECK(meta_stub->GetFile(&ctx, req, &meta).ok());
  }
  CHECK_EQ(meta.chunks(0).node_ids_size(), 3);
  const std::string chunk_id = meta.chunks(0).chunk().chunk_id();
  const std::string victim = meta.chunks(0).node_ids(0);

  // failure_threshold = 1 so a single round both detects the death and repairs it.
  MaintenanceOptions options;
  options.failure_threshold = 1;
  options.probe_timeout = std::chrono::milliseconds(500);
  options.replication_factor = 3;
  std::vector<std::string> logged;
  ClusterMaintenance maintenance(
      &meta_store, [&meta_service] { return meta_service.SnapshotRing(); },
      [&logged](const std::string& message) { logged.push_back(message); }, options);

  // Healthy cluster: a round changes nothing.
  {
    const MaintenanceReport report = maintenance.RunOnce();
    CHECK_EQ(report.heal.repaired, 0);
    CHECK_EQ(report.heal.under_replicated, 0);
    CHECK(maintenance.tracker().DeadNodes().empty());
  }

  // Kill a holder; one round should detect it AND restore the replication factor.
  for (const auto& node : nodes) {
    if (node->node_id == victim) node->server->Shutdown();
  }
  {
    const MaintenanceReport report = maintenance.RunOnce();
    CHECK_EQ(maintenance.tracker().DeadNodes(), (std::vector<std::string>{victim}));
    CHECK_EQ(report.heal.under_replicated, 1);
    CHECK_EQ(report.heal.repaired, 1);
  }

  // The new holder is a node that wasn't in the original placement.
  const std::vector<std::string> holders = meta_store.ChunkLocations(chunk_id);
  CHECK_EQ(holders.size(), 4u);  // 3 original (one now dead) + 1 fresh
  int live = 0;
  for (const std::string& holder : holders) {
    if (maintenance.tracker().IsAlive(holder)) ++live;
  }
  CHECK_EQ(live, 3);

  // The loop reported both the death and the repair.
  bool saw_down = false;
  bool saw_heal = false;
  for (const std::string& message : logged) {
    if (message.find("nodes down") != std::string::npos) saw_down = true;
    if (message.find("healed") != std::string::npos) saw_heal = true;
  }
  CHECK(saw_down);
  CHECK(saw_heal);

  // Converged: another round finds nothing to do.
  CHECK_EQ(maintenance.RunOnce().heal.repaired, 0);

  // Start()/Stop() must be safe even with no work to do.
  maintenance.Start();
  maintenance.Stop();

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

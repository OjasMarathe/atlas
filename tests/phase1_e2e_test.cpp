// Phase 1 end-to-end test — the Milestone-1 demo as an automated check.
//
// Spins up a metadata node + 4 storage nodes on loopback, then drives the real AtlasClient:
//   upload a file -> its chunk lands on 3 distinct nodes -> download reassembles it ->
//   kill a replica -> download STILL succeeds (read-around) -> re-upload -> versioning.

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "client/client.h"
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
  node->path = (fs::temp_directory_path() / ("atlas_e2e_" + id)).string();
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

std::string AddressOf(const std::vector<std::unique_ptr<StorageNode>>& nodes,
                      const std::string& node_id) {
  for (const auto& node : nodes) {
    if (node->node_id == node_id) return node->address;
  }
  return {};
}

// Direct GetChunk against one node, bypassing the client — used to confirm where replicas landed.
bool NodeHasChunk(const std::string& address, const std::string& chunk_id) {
  auto stub = atlas::StorageService::NewStub(
      grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
  grpc::ClientContext ctx;
  atlas::GetChunkRequest req;
  req.set_chunk_id(chunk_id);
  req.set_verify_checksum(true);
  auto reader = stub->GetChunk(&ctx, req);
  atlas::ChunkFrame frame;
  while (reader->Read(&frame)) {
  }
  return reader->Finish().ok();
}

}  // namespace

int main() {
  using namespace atlas;

  // --- metadata node ---
  const std::string meta_path = (fs::temp_directory_path() / "atlas_e2e_meta").string();
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

  // --- 4 storage nodes ---
  std::vector<std::unique_ptr<StorageNode>> nodes;
  for (int i = 1; i <= 4; ++i) nodes.push_back(StartStorageNode("storage-" + std::to_string(i)));

  // --- register the storage nodes into the ring ---
  {
    auto meta_stub = MetadataService::NewStub(
        grpc::CreateChannel(meta_address, grpc::InsecureChannelCredentials()));
    for (const auto& node : nodes) {
      grpc::ClientContext ctx;
      UpdateMembershipRequest req;
      RingState resp;
      req.mutable_node()->set_node_id(node->node_id);
      req.mutable_node()->set_address(node->address);
      req.set_change(UpdateMembershipRequest::JOIN);
      CHECK(meta_stub->UpdateMembership(&ctx, req, &resp).ok());
    }
  }

  AtlasClient client(meta_address);
  const std::string content =
      "Atlas distributed file system, end to end. " + std::string(1000, 'x');

  // --- upload ---
  const UploadResult up = client.Upload("hello.txt", "ojas", content);
  CHECK(up.ok);
  if (!up.ok) std::printf("upload failed: %s\n", up.message.c_str());
  CHECK_EQ(up.chunks, 1u);  // < 4 MiB -> a single chunk

  // --- the chunk is placed on 3 distinct nodes, and all 3 actually hold it ---
  FileMetadata meta;
  {
    auto meta_stub = MetadataService::NewStub(
        grpc::CreateChannel(meta_address, grpc::InsecureChannelCredentials()));
    grpc::ClientContext ctx;
    GetFileRequest req;
    req.set_file_id("hello.txt");
    CHECK(meta_stub->GetFile(&ctx, req, &meta).ok());
  }
  CHECK_EQ(meta.chunks_size(), 1);
  CHECK_EQ(meta.chunks(0).node_ids_size(), 3);
  const std::string chunk_id = meta.chunks(0).chunk().chunk_id();
  int holders = 0;
  for (const std::string& node_id : meta.chunks(0).node_ids()) {
    const std::string address = AddressOf(nodes, node_id);
    CHECK(!address.empty());
    if (NodeHasChunk(address, chunk_id)) ++holders;
  }
  CHECK_EQ(holders, 3);  // replication factor honored

  // --- download reassembles the exact bytes ---
  const DownloadResult down = client.Download("hello.txt");
  CHECK(down.ok);
  CHECK_EQ(down.data, content);

  // --- kill one replica, download STILL succeeds (read-around) ---
  const std::string dead = meta.chunks(0).node_ids(0);  // the primary
  for (const auto& node : nodes) {
    if (node->node_id == dead) node->Kill();
  }
  const DownloadResult after_death = client.Download("hello.txt");
  CHECK(after_death.ok);
  CHECK_EQ(after_death.data, content);
  std::printf("read-around: killed %s, still served the file from a surviving replica\n",
              dead.c_str());

  // --- re-upload creates a new version; download returns the latest (copy-on-write) ---
  const std::string content_v2 = "second version of the file";
  CHECK(client.Upload("hello.txt", "ojas", content_v2).ok);
  const DownloadResult latest = client.Download("hello.txt");
  CHECK(latest.ok);
  CHECK_EQ(latest.data, content_v2);
  const DownloadResult v1 = client.Download("hello.txt", /*version=*/1);
  CHECK(v1.ok);
  CHECK_EQ(v1.data, content);  // old version still reads its original bytes

  // --- cleanup ---
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

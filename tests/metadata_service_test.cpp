// gRPC integration test for MetadataServiceImpl over a loopback server (Phase 1 slice A).
// Exercises versioned RegisterFile/GetFile/ListVersions and ring membership over the wire.

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "metadata.grpc.pb.h"
#include "metadata/metadata_service.h"
#include "metadata/metadata_store.h"

namespace {
int g_checks = 0;
int g_fails = 0;
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

int main() {
  using namespace atlas;
  namespace fs = std::filesystem;
  const std::string dbpath = (fs::temp_directory_path() / "atlas_metadata_svc_test").string();
  fs::remove_all(dbpath);

  MetadataStore store(dbpath);
  CHECK(store.ok());
  MetadataServiceImpl svc(&store, /*vnodes_per_node=*/64);

  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&svc);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  CHECK(server != nullptr);

  auto channel =
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
  auto stub = MetadataService::NewStub(channel);

  // Ring starts empty.
  {
    grpc::ClientContext ctx;
    GetRingRequest req;
    RingState resp;
    CHECK(stub->GetRing(&ctx, req, &resp).ok());
    CHECK_EQ(resp.nodes_size(), 0);
  }

  // Join three storage nodes.
  struct NodeSpec {
    const char* id;
    const char* addr;
  };
  const NodeSpec kNodes[] = {{"storage-1", "127.0.0.1:6001"},
                             {"storage-2", "127.0.0.1:6002"},
                             {"storage-3", "127.0.0.1:6003"}};
  for (const NodeSpec& node : kNodes) {
    grpc::ClientContext ctx;
    UpdateMembershipRequest req;
    RingState resp;
    req.mutable_node()->set_node_id(node.id);
    req.mutable_node()->set_address(node.addr);
    req.set_change(UpdateMembershipRequest::JOIN);
    CHECK(stub->UpdateMembership(&ctx, req, &resp).ok());
  }
  {
    grpc::ClientContext ctx;
    GetRingRequest req;
    RingState resp;
    CHECK(stub->GetRing(&ctx, req, &resp).ok());
    CHECK_EQ(resp.nodes_size(), 3);
    CHECK_EQ(resp.virtual_nodes_per_node(), 64u);
    CHECK_EQ(resp.version(), 3u);  // three joins
  }

  // RegisterFile assigns incrementing versions; chunk placements round-trip.
  {
    grpc::ClientContext ctx;
    RegisterFileRequest req;
    FileMetadata resp;
    req.set_file_id("doc.txt");
    req.set_owner("ojas");
    req.set_replication_factor(3);
    ChunkPlacement* placement = req.add_chunks();
    placement->mutable_chunk()->set_chunk_id("abc123");
    placement->add_node_ids("storage-1");
    CHECK(stub->RegisterFile(&ctx, req, &resp).ok());
    CHECK_EQ(resp.version(), 1u);
  }
  {
    grpc::ClientContext ctx;
    RegisterFileRequest req;
    FileMetadata resp;
    req.set_file_id("doc.txt");
    CHECK(stub->RegisterFile(&ctx, req, &resp).ok());
    CHECK_EQ(resp.version(), 2u);
  }
  {
    grpc::ClientContext ctx;
    GetFileRequest req;
    FileMetadata resp;
    req.set_file_id("doc.txt");
    req.set_version(0);  // latest
    CHECK(stub->GetFile(&ctx, req, &resp).ok());
    CHECK_EQ(resp.version(), 2u);
  }
  {
    grpc::ClientContext ctx;
    GetFileRequest req;
    FileMetadata resp;
    req.set_file_id("doc.txt");
    req.set_version(1);
    CHECK(stub->GetFile(&ctx, req, &resp).ok());
    CHECK_EQ(resp.chunks_size(), 1);
    CHECK_EQ(resp.chunks(0).chunk().chunk_id(), std::string("abc123"));
  }
  {
    grpc::ClientContext ctx;
    ListVersionsRequest req;
    ListVersionsResponse resp;
    req.set_file_id("doc.txt");
    CHECK(stub->ListVersions(&ctx, req, &resp).ok());
    CHECK_EQ(resp.versions_size(), 2);
  }
  {
    grpc::ClientContext ctx;
    GetFileRequest req;
    FileMetadata resp;
    req.set_file_id("absent");
    CHECK(stub->GetFile(&ctx, req, &resp).error_code() == grpc::StatusCode::NOT_FOUND);
  }

  // Leave removes a node from the ring.
  {
    grpc::ClientContext ctx;
    UpdateMembershipRequest req;
    RingState resp;
    req.mutable_node()->set_node_id("storage-2");
    req.set_change(UpdateMembershipRequest::LEAVE);
    CHECK(stub->UpdateMembership(&ctx, req, &resp).ok());
    CHECK_EQ(resp.nodes_size(), 2);
  }

  server->Shutdown();
  fs::remove_all(dbpath);

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

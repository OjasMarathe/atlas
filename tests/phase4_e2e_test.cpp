// Phase 4 end-to-end, over a real cluster: metadata + 4 storage/search nodes + the ingestion
// client + the coordinator, all in-process but all talking over real gRPC.
//
// This is the test that would have caught the gap Phase 4 started from: before it, nothing in a
// running cluster ever called IndexDocument, so every shard was empty and a distributed query
// correctly merged four empty result sets. Here a document uploaded with `atlas put` must come
// back from a query that never mentions where it was stored.

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "client/client.h"
#include "coordinator/coordinator.h"
#include "coordinator/coordinator_service.h"
#include "metadata.grpc.pb.h"
#include "metadata/metadata_service.h"
#include "metadata/metadata_store.h"
#include "search/search_service.h"
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

// A node exactly as atlas_node runs it in the storage role: chunk store + search shard on one
// port, registered into the ring advertising both STORAGE and SEARCH.
struct Node {
  std::string node_id;
  std::string address;
  std::string path;
  std::unique_ptr<atlas::ChunkStore> store;
  std::unique_ptr<atlas::StorageServiceImpl> storage_service;
  std::unique_ptr<atlas::search::SearchServiceImpl> search_service;
  std::unique_ptr<grpc::Server> server;
};

std::unique_ptr<Node> StartNode(const std::string& id) {
  auto node = std::make_unique<Node>();
  node->node_id = id;
  node->path = (fs::temp_directory_path() / ("atlas_p4_" + id)).string();
  fs::remove_all(node->path);
  node->store = std::make_unique<atlas::ChunkStore>(node->path);
  node->storage_service = std::make_unique<atlas::StorageServiceImpl>(node->store.get());
  node->search_service = std::make_unique<atlas::search::SearchServiceImpl>();

  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(node->storage_service.get());
  builder.RegisterService(node->search_service.get());
  node->server = builder.BuildAndStart();
  node->address = "127.0.0.1:" + std::to_string(port);
  return node;
}

}  // namespace

int main() {
  using namespace atlas;
  using atlas::coordinator::CoordinatorOptions;
  using atlas::coordinator::QueryCoordinator;
  using atlas::coordinator::QueryResult;
  using atlas::coordinator::RingShardDirectory;

  // ---- control plane ----
  const std::string meta_path = (fs::temp_directory_path() / "atlas_p4_meta").string();
  fs::remove_all(meta_path);
  MetadataStore meta_store(meta_path);
  CHECK(meta_store.ok());
  MetadataServiceImpl meta_service(&meta_store);

  int meta_port = 0;
  grpc::ServerBuilder meta_builder;
  meta_builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &meta_port);
  meta_builder.RegisterService(&meta_service);
  std::unique_ptr<grpc::Server> meta_server = meta_builder.BuildAndStart();
  const std::string meta_address = "127.0.0.1:" + std::to_string(meta_port);
  auto meta_stub = MetadataService::NewStub(
      grpc::CreateChannel(meta_address, grpc::InsecureChannelCredentials()));

  // ---- 4 storage+search nodes, registering both roles like atlas_node does ----
  std::vector<std::unique_ptr<Node>> nodes;
  for (int i = 1; i <= 4; ++i) {
    nodes.push_back(StartNode("node" + std::to_string(i)));
    grpc::ClientContext ctx;
    UpdateMembershipRequest request;
    RingState response;
    request.mutable_node()->set_node_id(nodes.back()->node_id);
    request.mutable_node()->set_address(nodes.back()->address);
    request.mutable_node()->add_roles(atlas::STORAGE);
    request.mutable_node()->add_roles(atlas::SEARCH);
    request.set_change(UpdateMembershipRequest::JOIN);
    CHECK(meta_stub->UpdateMembership(&ctx, request, &response).ok());
  }

  // ---- shard discovery really does read SEARCH out of the ring ----
  RingShardDirectory directory(meta_address);
  const std::vector<coordinator::Shard> discovered = directory.Shards();
  CHECK_EQ(discovered.size(), 4u);

  // ---- ingest: uploading a document must also make it searchable ----
  AtlasClient client(meta_address);
  struct Doc {
    std::string file_id;
    std::string text;
  };
  const std::vector<Doc> documents = {
      {"consistent-hashing.md",
       "A consistent hashing ring maps chunks to nodes so that adding a node moves only its "
       "share of the keys, instead of remapping nearly everything the way hash modulo N does."},
      {"replication.md",
       "Every chunk is replicated onto three distinct nodes and acknowledged at a write quorum "
       "of two, so the cluster survives losing one replica without stalling writes."},
      {"bm25.md",
       "BM25 ranks documents by term frequency saturation and document length normalization, "
       "using an inverse document frequency that rewards rare terms."},
      {"self-healing.md",
       "When a node dies the healer re-replicates every chunk that fell below the replication "
       "factor, copying bytes from a surviving holder onto a fresh node chosen by the ring."},
      {"inverted-index.md",
       "An inverted index maps each term to a posting list of the documents containing it, "
       "which is what makes a term lookup cost far less than scanning every document."},
      {"wal.md",
       "A write-ahead log records an intended mutation durably before applying it, so a crash "
       "mid-write leaves a recoverable record rather than a torn state."},
  };

  for (const Doc& document : documents) {
    const UploadResult uploaded = client.Upload(document.file_id, "harshal", document.text);
    CHECK(uploaded.ok);
    CHECK(uploaded.indexed);  // the bridge Phase 4 added: stored *and* indexed
  }

  // Documents must be spread over more than one shard, or "distributed query" proves nothing.
  std::set<std::string> shards_holding_documents;
  for (const auto& node : nodes) {
    grpc::ClientContext ctx;
    StatsRequest request;
    ShardStats response;
    auto stub = SearchService::NewStub(
        grpc::CreateChannel(node->address, grpc::InsecureChannelCredentials()));
    CHECK(stub->Stats(&ctx, request, &response).ok());
    if (response.doc_count() > 0) shards_holding_documents.insert(node->node_id);
  }
  CHECK(shards_holding_documents.size() >= 2);
  std::printf("ingest: %zu documents spread over %zu of %zu shards\n", documents.size(),
              shards_holding_documents.size(), nodes.size());

  // ---- query through the coordinator, exactly as `atlas search` does ----
  QueryCoordinator coordinator([&directory] { return directory.Shards(); }, CoordinatorOptions{});
  coordinator::CoordinatorServiceImpl coordinator_service(&coordinator);

  int coordinator_port = 0;
  grpc::ServerBuilder coordinator_builder;
  coordinator_builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                                       &coordinator_port);
  coordinator_builder.RegisterService(&coordinator_service);
  std::unique_ptr<grpc::Server> coordinator_server = coordinator_builder.BuildAndStart();
  auto coordinator_stub = CoordinatorService::NewStub(grpc::CreateChannel(
      "127.0.0.1:" + std::to_string(coordinator_port), grpc::InsecureChannelCredentials()));

  {
    grpc::ClientContext ctx;
    QueryRequest request;
    request.set_query("consistent hashing ring");
    request.set_top_k(3);
    QueryResponse response;
    CHECK(coordinator_stub->Query(&ctx, request, &response).ok());
    CHECK_EQ(response.shards_queried(), 4u);
    CHECK_EQ(response.shards_responded(), 4u);
    CHECK(response.hits_size() > 0);
    // The document actually about consistent hashing must win, from wherever it landed.
    CHECK_EQ(response.hits(0).file_id(), std::string("consistent-hashing.md"));
    std::printf("query 'consistent hashing ring' -> %s (%.3f) across %u shards in %.2f ms\n",
                response.hits(0).file_id().c_str(), response.hits(0).score(),
                response.shards_responded(), response.latency_ms());
  }

  {
    // A different query must pick a different winner — proving ranking, not a fixed order.
    grpc::ClientContext ctx;
    QueryRequest request;
    request.set_query("write ahead log crash");
    request.set_top_k(3);
    QueryResponse response;
    CHECK(coordinator_stub->Query(&ctx, request, &response).ok());
    CHECK(response.hits_size() > 0);
    CHECK_EQ(response.hits(0).file_id(), std::string("wal.md"));
  }

  {
    // Warm cache, over the wire.
    grpc::ClientContext ctx;
    QueryRequest request;
    request.set_query("replication quorum");
    QueryResponse response;
    CHECK(coordinator_stub->Query(&ctx, request, &response).ok());
    CHECK(!response.cache_hit());

    grpc::ClientContext warm_ctx;
    QueryResponse warm;
    CHECK(coordinator_stub->Query(&warm_ctx, request, &warm).ok());
    CHECK(warm.cache_hit());
    CHECK(warm.latency_ms() < response.latency_ms());

    grpc::ClientContext stats_ctx;
    CacheStatsRequest stats_request;
    CacheStatsResponse stats;
    CHECK(coordinator_stub->CacheStats(&stats_ctx, stats_request, &stats).ok());
    CHECK_EQ(stats.hits(), 1u);
    CHECK_EQ(stats.policy(), std::string("lru"));
  }

  {
    // Uploaded binary data is stored but deliberately not indexed.
    std::string binary(512, '\0');
    binary[10] = 'x';
    const UploadResult uploaded = client.Upload("blob.bin", "harshal", binary);
    CHECK(uploaded.ok);
    CHECK(!uploaded.indexed);
  }

  {
    // The file is still a normal DFS file: Phase 4 changed ingestion without breaking reads.
    const DownloadResult downloaded = client.Download("wal.md");
    CHECK(downloaded.ok);
    CHECK_EQ(downloaded.data, documents[5].text);
  }

  coordinator_server->Shutdown();
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

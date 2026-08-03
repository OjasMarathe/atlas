// atlas — command-line client for a running Atlas cluster.
//
//   atlas put    <file_id> <path>        upload a file (and index it for search)
//   atlas get    <file_id> [out_path]    download it (stdout if no path)
//   atlas info   <file_id>               show chunks and which nodes hold them
//   atlas nodes                          show cluster membership
//   atlas search <query> [top_k]         ranked search across every shard (Phase 4)
//   atlas shards                         per-shard index sizes
//   atlas cache                          coordinator result-cache hit ratio
//
// Env: ATLAS_METADATA (default 127.0.0.1:50050) · ATLAS_COORDINATOR (default 127.0.0.1:50060)

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "client/client.h"
#include "coordinator.grpc.pb.h"
#include "metadata.grpc.pb.h"
#include "search.grpc.pb.h"

namespace {

std::string MetadataAddress() {
  const char* v = std::getenv("ATLAS_METADATA");
  return (v != nullptr && *v != '\0') ? std::string(v) : std::string("127.0.0.1:50050");
}

std::string CoordinatorAddress() {
  const char* v = std::getenv("ATLAS_COORDINATOR");
  return (v != nullptr && *v != '\0') ? std::string(v) : std::string("127.0.0.1:50060");
}

int Usage() {
  std::cerr << "usage:\n"
            << "  atlas put    <file_id> <path>\n"
            << "  atlas get    <file_id> [out_path]\n"
            << "  atlas info   <file_id>\n"
            << "  atlas nodes\n"
            << "  atlas search <query> [top_k]\n"
            << "  atlas shards\n"
            << "  atlas cache\n";
  return 2;
}

int Put(const std::string& file_id, const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "cannot read " << path << "\n";
    return 1;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string data = std::move(buffer).str();

  atlas::AtlasClient client(MetadataAddress());
  const atlas::UploadResult result =
      client.Upload(file_id, std::getenv("USER") != nullptr ? std::getenv("USER") : "atlas", data);
  if (!result.ok) {
    std::cerr << "upload failed: " << result.message << "\n";
    return 1;
  }
  std::cout << "uploaded " << file_id << " (" << data.size() << " bytes, " << result.chunks
            << " chunk(s), replicated 3x"
            << (result.indexed ? ", indexed for search" : ", not indexed") << ")\n";
  return 0;
}

int Get(const std::string& file_id, const std::string& out_path) {
  atlas::AtlasClient client(MetadataAddress());
  const atlas::DownloadResult result = client.Download(file_id);
  if (!result.ok) {
    std::cerr << "download failed: " << result.message << "\n";
    return 1;
  }
  if (out_path.empty()) {
    std::cout << result.data;
  } else {
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
      std::cerr << "cannot write " << out_path << "\n";
      return 1;
    }
    out << result.data;
    std::cout << "wrote " << result.data.size() << " bytes to " << out_path << "\n";
  }
  return 0;
}

std::unique_ptr<atlas::MetadataService::Stub> MetadataStub() {
  return atlas::MetadataService::NewStub(
      grpc::CreateChannel(MetadataAddress(), grpc::InsecureChannelCredentials()));
}

int Info(const std::string& file_id) {
  auto stub = MetadataStub();
  grpc::ClientContext ctx;
  atlas::GetFileRequest request;
  request.set_file_id(file_id);
  atlas::FileMetadata meta;
  const grpc::Status status = stub->GetFile(&ctx, request, &meta);
  if (!status.ok()) {
    std::cerr << "not found: " << file_id << " (" << status.error_message() << ")\n";
    return 1;
  }
  std::cout << file_id << "  version " << meta.version() << "  owner " << meta.owner() << "  "
            << meta.chunks_size() << " chunk(s)\n";
  for (const atlas::ChunkPlacement& placement : meta.chunks()) {
    std::cout << "  " << placement.chunk().chunk_id().substr(0, 12) << "…  " << std::setw(9)
              << placement.chunk().size() << " B  holders:";
    for (const std::string& node : placement.node_ids()) std::cout << " " << node;
    std::cout << "\n";
  }
  return 0;
}

int Nodes() {
  auto stub = MetadataStub();
  grpc::ClientContext ctx;
  atlas::GetRingRequest request;
  atlas::RingState state;
  if (!stub->GetRing(&ctx, request, &state).ok()) {
    std::cerr << "cannot reach metadata at " << MetadataAddress() << "\n";
    return 1;
  }
  std::cout << "ring version " << state.version() << ", " << state.nodes_size() << " node(s), "
            << state.virtual_nodes_per_node() << " vnodes each\n";
  for (const atlas::NodeInfo& node : state.nodes()) {
    std::cout << "  " << node.node_id() << "  " << node.address() << "\n";
  }
  return 0;
}

// Per-shard index sizes, straight from each SEARCH member's Stats RPC. This is what makes
// document partitioning visible: the counts should add up to the corpus, not repeat it.
int Shards() {
  auto stub = MetadataStub();
  grpc::ClientContext ctx;
  atlas::GetRingRequest request;
  atlas::RingState state;
  if (!stub->GetRing(&ctx, request, &state).ok()) {
    std::cerr << "cannot reach metadata at " << MetadataAddress() << "\n";
    return 1;
  }

  std::uint64_t total = 0;
  int shard_count = 0;
  for (const atlas::NodeInfo& node : state.nodes()) {
    bool is_shard = false;
    for (const int role : node.roles()) {
      if (role == atlas::SEARCH) is_shard = true;
    }
    if (!is_shard) continue;
    ++shard_count;

    auto search = atlas::SearchService::NewStub(
        grpc::CreateChannel(node.address(), grpc::InsecureChannelCredentials()));
    grpc::ClientContext shard_ctx;
    shard_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    atlas::StatsRequest stats_request;
    atlas::ShardStats stats;
    if (!search->Stats(&shard_ctx, stats_request, &stats).ok()) {
      std::cout << "  " << node.node_id() << "  " << node.address() << "  unreachable\n";
      continue;
    }
    total += stats.doc_count();
    std::cout << "  " << node.node_id() << "  " << node.address() << "  " << stats.doc_count()
              << " doc(s), " << stats.unique_terms() << " term(s), avgdl " << std::fixed
              << std::setprecision(1) << stats.avg_doc_len() << "\n";
  }
  std::cout << shard_count << " shard(s), " << total << " document(s) indexed in total\n";
  return 0;
}

std::unique_ptr<atlas::CoordinatorService::Stub> CoordinatorStub() {
  return atlas::CoordinatorService::NewStub(
      grpc::CreateChannel(CoordinatorAddress(), grpc::InsecureChannelCredentials()));
}

int Search(const std::string& query, std::uint32_t top_k) {
  auto stub = CoordinatorStub();
  grpc::ClientContext ctx;
  atlas::QueryRequest request;
  request.set_query(query);
  request.set_top_k(top_k);
  atlas::QueryResponse response;
  const grpc::Status status = stub->Query(&ctx, request, &response);
  if (!status.ok()) {
    std::cerr << "query failed: " << status.error_message() << " (coordinator at "
              << CoordinatorAddress() << ")\n";
    return 1;
  }

  std::cout << response.hits_size() << " hit(s) in " << std::fixed << std::setprecision(2)
            << response.latency_ms() << " ms across " << response.shards_responded() << "/"
            << response.shards_queried() << " shard(s)"
            << (response.cache_hit() ? "  [cache hit]" : "") << "\n";
  // A shard that missed its deadline is dropped, not retried — say so, because the result is
  // then a subset of the corpus and a caller may care.
  if (response.shards_responded() < response.shards_queried()) {
    std::cout << "  warning: partial results — "
              << (response.shards_queried() - response.shards_responded())
              << " shard(s) did not answer\n";
  }
  int rank = 1;
  for (const atlas::ScoredDoc& hit : response.hits()) {
    std::cout << "  " << rank++ << ". " << hit.file_id() << "  (" << std::setprecision(4)
              << hit.score() << ")\n";
    if (!hit.snippet().empty()) std::cout << "     " << hit.snippet() << "\n";
  }
  return 0;
}

int CacheInfo() {
  auto stub = CoordinatorStub();
  grpc::ClientContext ctx;
  atlas::CacheStatsRequest request;
  atlas::CacheStatsResponse response;
  if (!stub->CacheStats(&ctx, request, &response).ok()) {
    std::cerr << "cannot reach coordinator at " << CoordinatorAddress() << "\n";
    return 1;
  }
  const std::uint64_t total = response.hits() + response.misses();
  const double ratio =
      total == 0 ? 0.0 : 100.0 * static_cast<double>(response.hits()) / static_cast<double>(total);
  std::cout << response.policy() << " cache: " << response.size() << "/" << response.capacity()
            << " entries, " << response.hits() << " hit(s), " << response.misses() << " miss(es), "
            << response.evictions() << " eviction(s), " << std::fixed << std::setprecision(1)
            << ratio << "% hit ratio\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return Usage();

  const std::string& command = args[0];
  if (command == "put" && args.size() == 3) return Put(args[1], args[2]);
  if (command == "get" && (args.size() == 2 || args.size() == 3)) {
    return Get(args[1], args.size() == 3 ? args[2] : "");
  }
  if (command == "info" && args.size() == 2) return Info(args[1]);
  if (command == "nodes" && args.size() == 1) return Nodes();
  if (command == "search" && (args.size() == 2 || args.size() == 3)) {
    const std::uint32_t top_k =
        args.size() == 3 ? static_cast<std::uint32_t>(std::atoi(args[2].c_str())) : 10;
    return Search(args[1], top_k);
  }
  if (command == "shards" && args.size() == 1) return Shards();
  if (command == "cache" && args.size() == 1) return CacheInfo();
  return Usage();
}

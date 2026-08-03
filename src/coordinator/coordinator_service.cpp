#include "coordinator/coordinator_service.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace atlas::coordinator {

RingShardDirectory::RingShardDirectory(const std::string& metadata_address,
                                       std::chrono::milliseconds refresh_after)
    : metadata_(atlas::MetadataService::NewStub(
          grpc::CreateChannel(metadata_address, grpc::InsecureChannelCredentials()))),
      refresh_after_(refresh_after) {}

std::vector<Shard> RingShardDirectory::Shards() {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  if (ever_fetched_ && now - fetched_at_ < refresh_after_) return cached_;

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
  atlas::GetRingRequest request;
  atlas::RingState state;
  if (!metadata_->GetRing(&context, request, &state).ok()) {
    // Keep serving the last known membership. The control plane being briefly unreachable is
    // not a reason to answer every query with "no shards".
    return cached_;
  }

  std::vector<Shard> shards;
  for (const atlas::NodeInfo& node : state.nodes()) {
    for (const int role : node.roles()) {
      if (role == atlas::SEARCH) {
        shards.push_back(Shard{node.node_id(), node.address()});
        break;
      }
    }
  }
  // Stable order so a query's fan-out (and any log of it) is reproducible.
  std::sort(shards.begin(), shards.end(),
            [](const Shard& lhs, const Shard& rhs) { return lhs.node_id < rhs.node_id; });

  cached_ = std::move(shards);
  fetched_at_ = now;
  ever_fetched_ = true;
  return cached_;
}

grpc::Status CoordinatorServiceImpl::Query(grpc::ServerContext* /*context*/,
                                           const atlas::QueryRequest* request,
                                           atlas::QueryResponse* response) {
  if (request->query().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "query is required");
  }

  const QueryResult result =
      coordinator_->Query(request->query(), request->top_k(), request->bypass_cache());
  if (!result.error.empty()) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, result.error);
  }

  for (const QueryHit& hit : result.hits) {
    atlas::ScoredDoc* scored = response->add_hits();
    scored->set_file_id(hit.file_id);
    scored->set_score(hit.score);
    scored->set_snippet(hit.snippet);
  }
  response->set_shards_queried(static_cast<std::uint32_t>(result.shards_queried));
  response->set_shards_responded(static_cast<std::uint32_t>(result.shards_responded));
  response->set_latency_ms(result.latency_ms);
  response->set_cache_hit(result.cache_hit);
  return grpc::Status::OK;
}

grpc::Status CoordinatorServiceImpl::CacheStats(grpc::ServerContext* /*context*/,
                                                const atlas::CacheStatsRequest* /*request*/,
                                                atlas::CacheStatsResponse* response) {
  const cache::Stats stats = coordinator_->cache_stats();
  response->set_hits(stats.hits);
  response->set_misses(stats.misses);
  response->set_evictions(stats.evictions);
  response->set_size(stats.size);
  response->set_capacity(stats.capacity);
  response->set_policy(coordinator_->cache_policy());
  return grpc::Status::OK;
}

}  // namespace atlas::coordinator

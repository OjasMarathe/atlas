#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "coordinator.grpc.pb.h"
#include "coordinator/coordinator.h"
#include "metadata.grpc.pb.h"

namespace atlas::coordinator {

// Discovers search shards from the metadata service's ring: every member advertising Role::SEARCH.
// Cached briefly, because a query must not pay a control-plane round trip to learn where to go —
// and membership changes on the scale of node restarts, not queries.
class RingShardDirectory {
 public:
  explicit RingShardDirectory(const std::string& metadata_address,
                              std::chrono::milliseconds refresh_after = std::chrono::seconds(5));

  std::vector<Shard> Shards();

 private:
  std::unique_ptr<atlas::MetadataService::Stub> metadata_;
  std::chrono::milliseconds refresh_after_;
  std::mutex mutex_;
  std::vector<Shard> cached_;
  std::chrono::steady_clock::time_point fetched_at_{};
  bool ever_fetched_ = false;
};

// gRPC adapter over QueryCoordinator. Separate target from the coordinator core so the fan-out
// and merge logic can be tested without standing up a CoordinatorService.
class CoordinatorServiceImpl final : public atlas::CoordinatorService::Service {
 public:
  explicit CoordinatorServiceImpl(QueryCoordinator* coordinator) : coordinator_(coordinator) {}

  grpc::Status Query(grpc::ServerContext* context, const atlas::QueryRequest* request,
                     atlas::QueryResponse* response) override;

  grpc::Status CacheStats(grpc::ServerContext* context, const atlas::CacheStatsRequest* request,
                          atlas::CacheStatsResponse* response) override;

 private:
  QueryCoordinator* coordinator_;  // not owned
};

}  // namespace atlas::coordinator

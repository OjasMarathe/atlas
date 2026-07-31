#pragma once

#include <mutex>

#include <grpcpp/grpcpp.h>

#include "search.grpc.pb.h"
#include "search/search_engine.h"

namespace atlas::search {

// gRPC adapter over SearchEngine.
//
// Lives in its own target (atlas_search_service) so the engine itself stays free of any
// proto/gRPC dependency and can be unit-tested against a local corpus.
class SearchServiceImpl final : public atlas::SearchService::Service {
 public:
  grpc::Status IndexDocument(grpc::ServerContext* context,
                             const atlas::IndexDocumentRequest* request,
                             atlas::Status* response) override;

  grpc::Status Search(grpc::ServerContext* context, const atlas::SearchRequest* request,
                      atlas::SearchResponse* response) override;

  grpc::Status Suggest(grpc::ServerContext* context, const atlas::SuggestRequest* request,
                       atlas::SuggestResponse* response) override;

  grpc::Status Stats(grpc::ServerContext* context, const atlas::StatsRequest* request,
                     atlas::ShardStats* response) override;

 private:
  // gRPC serves each RPC on its own thread, and SearchEngine is not internally synchronized.
  // One mutex is the honest M1 answer; a reader-writer split (concurrent searches, exclusive
  // indexing) is the obvious upgrade once indexing is continuous.
  mutable std::mutex mutex_;
  SearchEngine engine_;
};

}  // namespace atlas::search

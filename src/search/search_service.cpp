#include "search/search_service.h"

#include <map>
#include <string>
#include <vector>

namespace atlas::search {

namespace {
constexpr std::uint32_t kDefaultTopK = 10;
constexpr std::uint32_t kDefaultSuggestions = 10;
}  // namespace

grpc::Status SearchServiceImpl::IndexDocument(grpc::ServerContext* /*context*/,
                                              const atlas::IndexDocumentRequest* request,
                                              atlas::Status* response) {
  // One error channel only: gRPC drops the response message when the status is non-OK, so
  // filling `response` here would be dead code. proto/README prefers gRPC status codes on the
  // wire, and an empty file_id is squarely a client error.
  if (request->file_id().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "file_id is required");
  }
  // Proto map fields iterate in unspecified order; std::map keeps the stored attributes stable.
  const std::map<std::string, std::string> fields(request->fields().begin(),
                                                  request->fields().end());
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    engine_.IndexDocument(request->file_id(), request->text(), fields);
  }
  response->set_code(atlas::Status::OK);
  return grpc::Status::OK;
}

grpc::Status SearchServiceImpl::Suggest(grpc::ServerContext* /*context*/,
                                        const atlas::SuggestRequest* request,
                                        atlas::SuggestResponse* response) {
  const std::uint32_t limit = request->limit() > 0 ? request->limit() : kDefaultSuggestions;
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const Completion& completion : engine_.Suggest(request->prefix(), limit)) {
    response->add_suggestions(completion.word);
  }
  return grpc::Status::OK;
}

grpc::Status SearchServiceImpl::Search(grpc::ServerContext* /*context*/,
                                       const atlas::SearchRequest* request,
                                       atlas::SearchResponse* response) {
  const std::uint32_t top_k = request->top_k() > 0 ? request->top_k() : kDefaultTopK;

  std::string error;
  std::vector<SearchHit> hits;
  ShardStatistics stats{};
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    hits = engine_.Search(request->query(), top_k, &error);
    stats = engine_.Stats();
  }
  if (!error.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error);
  }

  for (const SearchHit& hit : hits) {
    atlas::ScoredDoc* scored = response->add_hits();
    scored->set_file_id(hit.file_id);
    scored->set_score(hit.score);
    // snippet: needs byte offsets from the tokenizer to highlight the matched span — Phase 3b.
  }
  // The coordinator needs these to correct for per-shard BM25 statistics (see concepts/bm25.md).
  atlas::ShardStats* shard_stats = response->mutable_stats();
  shard_stats->set_doc_count(stats.document_count);
  shard_stats->set_unique_terms(stats.unique_terms);
  shard_stats->set_avg_doc_len(stats.average_document_length);
  return grpc::Status::OK;
}

grpc::Status SearchServiceImpl::Stats(grpc::ServerContext* /*context*/,
                                      const atlas::StatsRequest* /*request*/,
                                      atlas::ShardStats* response) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const ShardStatistics stats = engine_.Stats();
  response->set_doc_count(stats.document_count);
  response->set_unique_terms(stats.unique_terms);
  response->set_avg_doc_len(stats.average_document_length);
  return grpc::Status::OK;
}

}  // namespace atlas::search

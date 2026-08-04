#include "coordinator/coordinator.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include <grpcpp/alarm.h>

#include "common/cache/lfu_cache.h"
#include "common/cache/lru_cache.h"
#include "search/search_engine.h"

namespace atlas::coordinator {
namespace {

// One in-flight async unary RPC. Everything the completion queue will write into must outlive
// the call, so context, response and status all live here rather than on the issuing stack.
template <typename Response>
struct AsyncCall {
  grpc::ClientContext context;
  Response response;
  grpc::Status status;
  ConnectionPool<atlas::SearchService>::Lease lease;  // held until the RPC completes
  std::unique_ptr<grpc::ClientAsyncResponseReader<Response>> reader;
};

// The cache key must pin every input that changes the answer. top_k is the easy one to forget:
// without it a cached top-3 would happily satisfy a later request for top-10.
std::string CacheKey(const std::string& query, std::size_t top_k, bool global_scoring) {
  return query + "\x1f" + std::to_string(top_k) + "\x1f" + (global_scoring ? "g" : "l");
}

}  // namespace

QueryCoordinator::QueryCoordinator(ShardSnapshot shards, CoordinatorOptions options)
    : options_(std::move(options)),
      shards_(std::move(shards)),
      pool_(options_.connections_per_shard) {
  if (options_.cache_policy == "lfu") {
    cache_ = std::make_unique<cache::LfuCache<std::string, QueryResult>>(options_.cache_capacity);
  } else {
    cache_ = std::make_unique<cache::LruCache<std::string, QueryResult>>(options_.cache_capacity);
  }
}

bool QueryCoordinator::CollectGlobalStats(const std::vector<Shard>& shards,
                                          const std::vector<std::string>& terms,
                                          atlas::GlobalStats* out) {
  atlas::TermStatsRequest request;
  for (const std::string& term : terms) request.add_terms(term);

  grpc::CompletionQueue queue;
  std::vector<std::unique_ptr<AsyncCall<atlas::TermStatsResponse>>> calls;
  calls.reserve(shards.size());

  const auto deadline = std::chrono::system_clock::now() + options_.shard_timeout;
  for (const Shard& shard : shards) {
    auto call = std::make_unique<AsyncCall<atlas::TermStatsResponse>>();
    call->lease = pool_.Acquire(shard.address);
    call->context.set_deadline(deadline);
    call->reader = call->lease->PrepareAsyncTermStats(&call->context, request, &queue);
    call->reader->StartCall();
    call->reader->Finish(&call->response, &call->status, call.get());
    calls.push_back(std::move(call));
  }

  // Every context carries a deadline, so each call is guaranteed to complete one way or the
  // other — waiting for exactly `calls.size()` completions cannot hang.
  std::uint64_t total_documents = 0;
  double weighted_length = 0.0;
  std::unordered_map<std::string, std::uint64_t> frequencies;
  int answered = 0;

  for (std::size_t completed = 0; completed < calls.size(); ++completed) {
    void* tag = nullptr;
    bool ok = false;
    if (!queue.Next(&tag, &ok)) break;
    auto* call = static_cast<AsyncCall<atlas::TermStatsResponse>*>(tag);
    if (!ok || !call->status.ok()) continue;

    ++answered;
    total_documents += call->response.doc_count();
    // avgdl is a per-document mean, so it recombines weighted by each shard's document count —
    // averaging the averages would over-weight a shard holding three documents.
    weighted_length +=
        call->response.avg_doc_len() * static_cast<double>(call->response.doc_count());
    for (const auto& [term, frequency] : call->response.document_frequency()) {
      frequencies[term] += frequency;
    }
  }
  queue.Shutdown();
  void* drain_tag = nullptr;
  bool drain_ok = false;
  while (queue.Next(&drain_tag, &drain_ok)) {
  }

  // Every shard must answer. Statistics gathered from a subset are not corpus statistics: a
  // missing shard shrinks N and avgdl and undercounts n(t) for every term, so the IDF applied to
  // all shards is wrong — and silently, since the scores still look plausible. Better to fall
  // back to shard-local scoring, which is at least a known approximation.
  if (answered != static_cast<int>(shards.size()) || total_documents == 0) return false;

  out->set_doc_count(total_documents);
  out->set_avg_doc_len(weighted_length / static_cast<double>(total_documents));
  for (const auto& [term, frequency] : frequencies) {
    (*out->mutable_document_frequency())[term] = frequency;
  }
  return true;
}

int QueryCoordinator::FanOut(const std::vector<Shard>& shards, const std::string& query,
                             std::size_t top_k, const atlas::GlobalStats* global,
                             std::vector<atlas::SearchResponse>* responses) {
  atlas::SearchRequest request;
  request.set_query(query);
  // Ask each shard for a full top_k, not top_k/shards: the global winners may all live on one
  // shard, and a shard cannot know how its documents rank against another's.
  request.set_top_k(static_cast<std::uint32_t>(top_k));
  if (global != nullptr) *request.mutable_global_stats() = *global;

  grpc::CompletionQueue queue;
  std::vector<std::unique_ptr<AsyncCall<atlas::SearchResponse>>> calls;
  calls.reserve(shards.size());

  const auto deadline = std::chrono::system_clock::now() + options_.shard_timeout;
  for (const Shard& shard : shards) {
    auto call = std::make_unique<AsyncCall<atlas::SearchResponse>>();
    call->lease = pool_.Acquire(shard.address);
    call->context.set_deadline(deadline);
    call->reader = call->lease->PrepareAsyncSearch(&call->context, request, &queue);
    call->reader->StartCall();
    call->reader->Finish(&call->response, &call->status, call.get());
    calls.push_back(std::move(call));
  }

  // This is the actual scatter-gather: one thread now has `shards.size()` RPCs in flight and
  // collects them as they land, so the fan-out costs the slowest shard rather than the sum.
  int responded = 0;
  for (std::size_t completed = 0; completed < calls.size(); ++completed) {
    void* tag = nullptr;
    bool ok = false;
    if (!queue.Next(&tag, &ok)) break;
    auto* call = static_cast<AsyncCall<atlas::SearchResponse>*>(tag);
    if (!ok || !call->status.ok()) continue;  // dropped: partial results beat a hung query
    ++responded;
    responses->push_back(std::move(call->response));
  }
  queue.Shutdown();
  void* drain_tag = nullptr;
  bool drain_ok = false;
  while (queue.Next(&drain_tag, &drain_ok)) {
  }
  return responded;
}

QueryResult QueryCoordinator::Query(const std::string& query, std::size_t top_k,
                                    bool bypass_cache) {
  const auto started = std::chrono::steady_clock::now();
  const auto elapsed_ms = [&started] {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
        .count();
  };

  if (top_k == 0) top_k = 10;
  const std::string key = CacheKey(query, top_k, options_.global_scoring);

  if (!bypass_cache) {
    if (std::optional<QueryResult> cached = cache_->Get(key)) {
      QueryResult result = std::move(*cached);
      result.cache_hit = true;
      result.latency_ms = elapsed_ms();  // the point of the cache: this is the number that drops
      return result;
    }
  }

  QueryResult result;
  const std::vector<Shard> shards = shards_();
  result.shards_queried = static_cast<int>(shards.size());
  if (shards.empty()) {
    result.error = "no search shards registered";
    result.latency_ms = elapsed_ms();
    return result;
  }

  // Round 1: corpus-wide statistics, so shard scores are comparable when merged.
  atlas::GlobalStats global;
  bool have_global = false;
  bool global_wanted = false;  // a query with terms to score, on a globally-scored coordinator
  if (options_.global_scoring) {
    const std::vector<std::string> terms = search::SearchEngine::QueryTerms(query);
    if (!terms.empty()) {
      global_wanted = true;
      have_global = CollectGlobalStats(shards, terms, &global);
    }
  }

  // Round 2: the query itself.
  std::vector<atlas::SearchResponse> responses;
  responses.reserve(shards.size());
  result.shards_responded =
      FanOut(shards, query, top_k, have_global ? &global : nullptr, &responses);

  // Merge. Deduplicate by file_id first: a document should live on exactly one shard
  // (ADR-0006), but a re-index during a placement change can transiently double it, and
  // showing the same document twice is worse than the extra pass costs.
  std::unordered_map<std::string, QueryHit> best;
  for (const atlas::SearchResponse& response : responses) {
    for (const atlas::ScoredDoc& hit : response.hits()) {
      auto [it, inserted] =
          best.try_emplace(hit.file_id(), QueryHit{hit.file_id(), hit.score(), hit.snippet()});
      if (!inserted && hit.score() > it->second.score) {
        it->second.score = hit.score();
        it->second.snippet = hit.snippet();
      }
    }
  }

  result.hits.reserve(best.size());
  for (auto& [file_id, hit] : best) result.hits.push_back(std::move(hit));

  // Highest score first, file_id ascending to break ties — without a deterministic tie-break the
  // same query can return different orderings run to run, since shards answer in race order.
  const std::size_t keep = std::min(top_k, result.hits.size());
  std::partial_sort(result.hits.begin(), result.hits.begin() + static_cast<std::ptrdiff_t>(keep),
                    result.hits.end(), [](const QueryHit& lhs, const QueryHit& rhs) {
                      if (lhs.score != rhs.score) return lhs.score > rhs.score;
                      return lhs.file_id < rhs.file_id;
                    });
  result.hits.resize(keep);

  result.latency_ms = elapsed_ms();

  // Only cache a complete answer. Caching a partial result would pin one shard's outage into
  // every later query for as long as the entry survives — and that applies to the statistics
  // round too: a result that fell back to shard-local scoring is cached under the same key a
  // globally-scored one would use, so it would keep serving the approximation long after the
  // stats round started succeeding again.
  if (result.shards_responded == result.shards_queried && (!global_wanted || have_global)) {
    QueryResult cacheable = result;
    cacheable.cache_hit = false;
    cacheable.latency_ms = 0.0;
    cache_->Put(key, std::move(cacheable));
  }
  return result;
}

cache::Stats QueryCoordinator::cache_stats() const { return cache_->stats(); }

std::string QueryCoordinator::cache_policy() const { return cache_->policy(); }

void QueryCoordinator::ClearCache() { cache_->Clear(); }

}  // namespace atlas::coordinator

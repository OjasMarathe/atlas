// Phase 4 DoD: a query hits >=3 shards in parallel, the results merge correctly, and a warm
// cache is measurably faster.
//
// The strongest assertion here is the global-scoring one: the same corpus is indexed twice —
// once split across four shards, once into a single local engine — and the distributed ranking
// must match the single-index ranking exactly. That is what ADR-0010's extra round trip buys,
// and the corpus is deliberately skewed so that shard-local IDF would get it wrong.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/pool/thread_pool.h"
#include "coordinator/coordinator.h"
#include "search/search_engine.h"
#include "search/search_service.h"

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

namespace {

struct ShardNode {
  std::unique_ptr<atlas::search::SearchServiceImpl> service;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<atlas::SearchService::Stub> stub;
  std::string node_id;
  std::string address;
};

std::unique_ptr<ShardNode> StartShard(const std::string& id) {
  auto shard = std::make_unique<ShardNode>();
  shard->node_id = id;
  shard->service = std::make_unique<atlas::search::SearchServiceImpl>();
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(shard->service.get());
  shard->server = builder.BuildAndStart();
  shard->address = "127.0.0.1:" + std::to_string(port);
  shard->stub = atlas::SearchService::NewStub(
      grpc::CreateChannel(shard->address, grpc::InsecureChannelCredentials()));
  return shard;
}

bool IndexOn(ShardNode* shard, const std::string& file_id, const std::string& text) {
  grpc::ClientContext context;
  atlas::IndexDocumentRequest request;
  request.set_file_id(file_id);
  request.set_text(text);
  atlas::Status response;
  return shard->stub->IndexDocument(&context, request, &response).ok();
}

struct Document {
  std::string file_id;
  std::string text;
  int shard;  // which shard owns it — assigned deliberately, to skew per-shard statistics
};

// 24 documents over 4 shards. "replication" is concentrated on shard 0 and "index" on shard 1,
// so each shard's local view of how common those words are is badly wrong. A merge of raw
// shard-local scores ranks by that wrong view; a DFS merge does not.
std::vector<Document> Corpus() {
  std::vector<Document> documents;
  for (int i = 0; i < 6; ++i) {
    documents.push_back({"repl-" + std::to_string(i),
                         "replication replication chunk placement across the storage ring", 0});
  }
  for (int i = 0; i < 6; ++i) {
    documents.push_back(
        {"index-" + std::to_string(i), "inverted index posting list index term lookup", 1});
  }
  // The document that should win a "replication ring" query: it is the only one pairing both
  // terms, and it lives on a shard where neither term looks common.
  documents.push_back({"winner", "replication ring placement replication ring quorum", 2});
  documents.push_back({"ring-only", "consistent hashing ring virtual nodes", 2});
  documents.push_back({"chunk-notes", "chunk store checksum durable write", 2});
  documents.push_back({"bm25-notes", "ranking bm25 scoring saturation", 2});
  for (int i = 0; i < 8; ++i) {
    documents.push_back({"misc-" + std::to_string(i),
                         "cluster node heartbeat failure detection maintenance loop", 3});
  }
  return documents;
}

}  // namespace

int main() {
  using atlas::coordinator::CoordinatorOptions;
  using atlas::coordinator::QueryCoordinator;
  using atlas::coordinator::QueryResult;
  using atlas::coordinator::Shard;

  constexpr int kShardCount = 4;
  std::vector<std::unique_ptr<ShardNode>> shards;
  for (int i = 0; i < kShardCount; ++i) shards.push_back(StartShard("shard" + std::to_string(i)));

  const std::vector<Document> corpus = Corpus();
  for (const Document& document : corpus) {
    CHECK(IndexOn(shards[document.shard].get(), document.file_id, document.text));
  }

  // The same corpus in one index — the reference ranking a distributed engine should reproduce.
  atlas::search::SearchEngine reference;
  for (const Document& document : corpus) reference.IndexDocument(document.file_id, document.text);

  std::vector<Shard> membership;
  for (const auto& shard : shards) membership.push_back(Shard{shard->node_id, shard->address});
  const auto snapshot = [&membership] { return membership; };

  // ---- the DoD's headline: a query reaches every shard, in parallel ----
  {
    CoordinatorOptions options;
    options.global_scoring = true;
    QueryCoordinator coordinator(snapshot, options);

    const QueryResult result = coordinator.Query("replication ring", 5);
    CHECK_EQ(result.shards_queried, kShardCount);
    CHECK_EQ(result.shards_responded, kShardCount);
    CHECK(result.shards_queried >= 3);  // the roadmap's literal bar
    CHECK(!result.hits.empty());
    CHECK(result.hits.size() <= 5u);

    // Merged, not concatenated: hits come from more than one shard and are score-ordered.
    for (std::size_t i = 1; i < result.hits.size(); ++i) {
      CHECK(result.hits[i - 1].score >= result.hits[i].score);
    }
    CHECK_EQ(result.hits[0].file_id, std::string("winner"));
    std::printf("fan-out: %d/%d shards, %zu hits, %.2f ms, top=%s\n", result.shards_responded,
                result.shards_queried, result.hits.size(), result.latency_ms,
                result.hits[0].file_id.c_str());
  }

  // ---- global scoring reproduces the single-index ranking exactly ----
  {
    CoordinatorOptions options;
    options.global_scoring = true;
    QueryCoordinator coordinator(snapshot, options);

    for (const std::string& query : {"replication ring", "index term lookup", "chunk placement"}) {
      const QueryResult distributed = coordinator.Query(query, 8, /*bypass_cache=*/true);
      const std::vector<atlas::search::SearchHit> single = reference.Search(query, 8);

      CHECK_EQ(distributed.hits.size(), single.size());
      const std::size_t compared = std::min(distributed.hits.size(), single.size());
      for (std::size_t i = 0; i < compared; ++i) {
        CHECK_EQ(distributed.hits[i].file_id, single[i].file_id);
        // Same statistics in, same score out — not merely the same order.
        CHECK(std::abs(distributed.hits[i].score - single[i].score) < 1e-9);
      }
    }
  }

  // ---- and without it, the shard-local approximation really is different ----
  //
  // This is why ADR-0010 spends a round trip. If these agreed, the correction would be
  // ceremony; the test would then be telling us to delete it.
  {
    CoordinatorOptions options;
    options.global_scoring = false;
    QueryCoordinator local(snapshot, options);

    const QueryResult approximate = local.Query("replication ring", 8);
    const std::vector<atlas::search::SearchHit> single = reference.Search("replication ring", 8);
    bool differs = false;
    const std::size_t compared = std::min(approximate.hits.size(), single.size());
    for (std::size_t i = 0; i < compared; ++i) {
      if (std::abs(approximate.hits[i].score - single[i].score) > 1e-9) differs = true;
    }
    CHECK(differs);
    std::printf("shard-local scoring diverges from the single-index ranking: %s\n",
                differs ? "yes (as expected)" : "no");
  }

  // ---- top_k is respected across the merge, not per shard ----
  {
    QueryCoordinator coordinator(snapshot, CoordinatorOptions{});
    const QueryResult result = coordinator.Query("replication", 3);
    CHECK_EQ(result.hits.size(), 3u);
    // More than 3 documents contain the term, so this really is a truncation of a larger merge.
    CHECK(reference.Search("replication", 100).size() > 3u);
  }

  // ---- a document indexed on two shards appears once ----
  {
    CHECK(IndexOn(shards[3].get(), "winner", "replication ring placement replication ring quorum"));
    QueryCoordinator coordinator(snapshot, CoordinatorOptions{});
    const QueryResult result = coordinator.Query("replication ring", 10);
    int occurrences = 0;
    for (const auto& hit : result.hits) {
      if (hit.file_id == "winner") ++occurrences;
    }
    CHECK_EQ(occurrences, 1);
  }

  // ---- warm cache: a repeat query is served without touching a shard ----
  {
    QueryCoordinator coordinator(snapshot, CoordinatorOptions{});
    const QueryResult cold = coordinator.Query("consistent hashing", 5);
    CHECK(!cold.cache_hit);
    CHECK_EQ(coordinator.cache_stats().misses, 1u);

    const QueryResult warm = coordinator.Query("consistent hashing", 5);
    CHECK(warm.cache_hit);
    CHECK_EQ(coordinator.cache_stats().hits, 1u);
    CHECK_EQ(warm.hits.size(), cold.hits.size());
    // Measurably faster is the DoD's word: a cache hit skips two network rounds entirely.
    CHECK(warm.latency_ms < cold.latency_ms);
    std::printf("cache: cold %.3f ms -> warm %.3f ms (%.0fx)\n", cold.latency_ms, warm.latency_ms,
                warm.latency_ms > 0 ? cold.latency_ms / warm.latency_ms : 0.0);

    // top_k is part of the cache key: a cached top-5 must not satisfy a top-10.
    const QueryResult wider = coordinator.Query("consistent hashing", 10);
    CHECK(!wider.cache_hit);

    // bypass_cache forces the fan-out even when an entry exists.
    const QueryResult forced = coordinator.Query("consistent hashing", 5, /*bypass_cache=*/true);
    CHECK(!forced.cache_hit);

    coordinator.ClearCache();
    CHECK_EQ(coordinator.cache_stats().size, 0u);
  }

  // ---- a dead shard degrades to partial results instead of failing the query ----
  {
    QueryCoordinator coordinator(snapshot, CoordinatorOptions{});
    shards[3]->server->Shutdown();
    shards[3]->server.reset();

    const QueryResult result = coordinator.Query("replication ring", 5, /*bypass_cache=*/true);
    CHECK_EQ(result.shards_queried, kShardCount);
    CHECK_EQ(result.shards_responded, kShardCount - 1);
    CHECK(!result.hits.empty());  // the surviving shards still answer

    // A partial answer must not be cached, or one shard's outage outlives it.
    const QueryResult repeat = coordinator.Query("replication ring", 5);
    CHECK(!repeat.cache_hit);
  }

  // ---- N concurrent clients (the load-test half of the DoD) ----
  //
  // Over the *surviving* shards: the block above killed one, and a coordinator returning partial
  // results deliberately refuses to cache them, which would leave this measuring an uncacheable
  // workload rather than a representative one.
  {
    std::vector<Shard> live;
    for (const auto& shard : shards) {
      if (shard->server) live.push_back(Shard{shard->node_id, shard->address});
    }
    QueryCoordinator coordinator([&live] { return live; }, CoordinatorOptions{});
    constexpr int kClients = 32;
    constexpr int kQueriesPerClient = 8;
    const std::vector<std::string> queries = {"replication ring", "inverted index", "chunk store",
                                              "bm25 ranking", "heartbeat failure"};

    atlas::ThreadPool clients(kClients);
    std::atomic<int> succeeded{0};
    std::atomic<int> failed{0};

    const auto started = std::chrono::steady_clock::now();
    std::vector<std::future<void>> running;
    running.reserve(kClients);
    for (int client = 0; client < kClients; ++client) {
      running.push_back(clients.Submit([&, client] {
        for (int q = 0; q < kQueriesPerClient; ++q) {
          const QueryResult result = coordinator.Query(queries[(client + q) % queries.size()], 5);
          if (result.error.empty() && result.shards_responded > 0) {
            ++succeeded;
          } else {
            ++failed;
          }
        }
      }));
    }
    for (auto& future : running) future.get();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();

    const int total = kClients * kQueriesPerClient;
    CHECK_EQ(succeeded.load(), total);
    CHECK_EQ(failed.load(), 0);
    std::printf("load: %d queries from %d concurrent clients in %.0f ms (%.0f q/s), cache %.0f%%\n",
                total, kClients, elapsed_ms, total / (elapsed_ms / 1000.0),
                coordinator.cache_stats().hit_ratio() * 100.0);
  }

  for (auto& shard : shards) {
    if (shard->server) shard->server->Shutdown();
  }

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

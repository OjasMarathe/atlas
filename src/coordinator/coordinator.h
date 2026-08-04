#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/cache/cache.h"
#include "common/pool/connection_pool.h"
#include "search.grpc.pb.h"

namespace atlas::coordinator {

struct Shard {
  std::string node_id;
  std::string address;
};

struct QueryHit {
  std::string file_id;
  double score = 0.0;
  std::string snippet;
};

struct QueryResult {
  std::vector<QueryHit> hits;
  int shards_queried = 0;
  int shards_responded = 0;  // < queried means partial results: a shard was slow or dead
  double latency_ms = 0.0;
  bool cache_hit = false;
  std::string error;
};

struct CoordinatorOptions {
  std::size_t cache_capacity = 256;
  std::string cache_policy = "lru";  // "lru" | "lfu" — see concepts/lru-lfu-cache.md
  std::chrono::milliseconds shard_timeout{1500};
  std::size_t connections_per_shard = 4;

  // Two-round DFS scoring. Costs one extra round trip; buys rankings that don't depend on which
  // shard a document happens to live on (ADR-0010). Off => single round, shard-local scores.
  bool global_scoring = true;
};

// The client-facing query entry point: parse-free fan-out to every search shard, merge their
// local top-Ks into one global top-K, and cache the answer.
//
// Shard discovery is injected rather than looked up here, mirroring ClusterMaintenance's
// RingSnapshot: the coordinator does not care whether membership comes from the metadata
// service, a config file, or a test fixture, and injecting it means the fan-out is testable
// against in-process shards with no control plane running.
//
// Partial results are a feature, not an error. A shard that misses its deadline is dropped and
// reported via shards_responded — for search, stale-but-fast beats correct-but-hung. See
// docs/concepts/scatter-gather.md.
class QueryCoordinator {
 public:
  using ShardSnapshot = std::function<std::vector<Shard>()>;

  QueryCoordinator(ShardSnapshot shards, CoordinatorOptions options = {});

  QueryResult Query(const std::string& query, std::size_t top_k, bool bypass_cache = false);

  cache::Stats cache_stats() const;
  std::string cache_policy() const;
  ConnectionPoolStats connection_stats() const { return pool_.stats(); }
  void ClearCache();

 private:
  // Round 1: ask every shard for its n(t) over `terms` and sum into corpus-wide statistics.
  // Returns false when no shard answered, in which case round 2 falls back to local scoring.
  bool CollectGlobalStats(const std::vector<Shard>& shards, const std::vector<std::string>& terms,
                          atlas::GlobalStats* out);

  // Round 2 (or the only round): fan the query out and gather each shard's local top-K.
  int FanOut(const std::vector<Shard>& shards, const std::string& query, std::size_t top_k,
             const atlas::GlobalStats* global, std::vector<atlas::SearchResponse>* responses);

  CoordinatorOptions options_;
  ShardSnapshot shards_;
  ConnectionPool<atlas::SearchService> pool_;
  std::unique_ptr<cache::Cache<std::string, QueryResult>> cache_;
};

}  // namespace atlas::coordinator

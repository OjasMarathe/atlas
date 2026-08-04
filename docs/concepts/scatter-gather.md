# Scatter-Gather (distributed query)

**Area:** Query · **Phase:** 4 · **Status:** written

## TL;DR

The index is split across shards, so no single node can answer a query. The **coordinator**
scatters the query to every shard in parallel, gathers their local top-Ks, and merges them into
one global top-K. The two things that make it more than a `for` loop are that the fan-out must be
concurrent (or latency becomes the *sum* of the shards) and that shard scores must be made
comparable before they can be merged at all.

## The problem it solves

Atlas partitions the search index **by document** (ADR-0006): each shard indexes the documents
whose chunks live on its co-located storage node. That is the right partitioning — indexing is
local, a document's postings never span nodes, and adding a node adds capacity — but it means
every shard holds a *different slice of the corpus* and none of them can rank the whole thing.

The naive coordinator:

```
hits = []
for shard in shards:            # sequential
    hits += shard.Search(q, k)  # each waits for the last
sort(hits); return hits[:k]
```

Two things are wrong with it, and they are the whole subject of this note.

**1. Latency is the sum, not the max.** With 4 shards at 5 ms each, that loop costs 20 ms. Fanned
out concurrently it costs ~5 ms. Worse, the sequential version gets *slower as you add shards* —
exactly backwards, since adding shards is how the system is supposed to scale.

**2. The scores aren't comparable.** BM25's IDF is `ln(1 + (N - n(t) + 0.5)/(n(t) + 0.5))`, where
`N` is the number of documents and `n(t)` how many contain the term. Computed per shard, both are
*local* numbers. A term appearing in 8 of one shard's 10 documents looks common there and rare on a
shard holding 10 documents without it — so the same document earns a different score depending on
which shard it happens to sit on. Sorting those numbers together is sorting apples and oranges.

## How it works

### Round 1 — make the scores comparable (DFS)

Before ranking, the coordinator collects the collection statistics from every shard and sums them:

```
N_global      = Σ N_i                        over shards i
n(t)_global   = Σ n_i(t)                     for each query term t
avgdl_global  = Σ (N_i · avgdl_i) / N_global  ← weighted by document count, not a mean of means
```

That last line is the one people get wrong. `avgdl` is a per-document average, so recombining it
requires weighting by how many documents each shard contributed; averaging the four averages would
let a shard holding 3 documents count as much as one holding 300.

### Round 2 — the query itself

The coordinator ships those global numbers back with the query. Each shard ranks its own documents
using **global** `N`, `n(t)` and `avgdl`, but **local** term frequency and document length — which
is correct, because tf and `|d|` are properties of the document itself, not of the corpus around
it. Now every score is the score that document would have received from one big index.

```
                   ┌──────── shard 1 ──── local top-K ────┐
   query ──▶ coordinator ── shard 2 ──── local top-K ──── merge ──▶ global top-K
                   ├──────── shard 3 ──── local top-K ────┤
                   └──────── shard 4 ──── (timeout) ──────┘   partial: 3/4 responded
```

### Why each shard is asked for a full K

Not `K/shards`. The global top-10 may all live on one shard — the coordinator cannot know before
asking, and a shard cannot know how its documents compare to another's. Asking each for K
guarantees the true global top-K is contained in the union. The cost is `shards × K` documents
transferred to select K; for a search UI's K of 10-50 that is trivially cheap, and it is the
standard trade (Elasticsearch does the same).

### Merging

Union the shards' hits, deduplicate by `file_id`, sort by score descending with `file_id`
ascending as a tie-break, truncate to K. The tie-break is not cosmetic: shards answer in race
order, so without it the same query returns different orderings run to run.

## Our implementation in Atlas

- **Where it lives:** [`src/coordinator/coordinator.cpp`](../../src/coordinator/coordinator.cpp)
  — `CollectGlobalStats` (round 1), `FanOut` (round 2), `Query` (merge + cache).
  [`src/coordinator/coordinator_service.cpp`](../../src/coordinator/coordinator_service.cpp) is
  the gRPC adapter plus `RingShardDirectory`, which discovers shards by filtering the ring for
  members advertising `Role::SEARCH`.
- **The fan-out is asynchronous**, not thread-per-shard. One thread issues every RPC through a
  gRPC `CompletionQueue` and collects them as they land — see
  [async-io](async-io.md) for why that beats blocking calls on a pool here.
- **Shard discovery is injected** (`ShardSnapshot`, a `std::function`), mirroring
  `ClusterMaintenance`'s `RingSnapshot`. The coordinator does not care whether membership comes
  from the metadata service or a test fixture, which is what lets `coordinator_test` run a real
  4-shard fan-out with no control plane at all.
- **Global scoring is a flag** (`CoordinatorOptions::global_scoring`, default on). Off, the
  coordinator does one round and merges shard-local scores.

The core of round 2:

```cpp
for (const Shard& shard : shards) {                 // scatter
  auto call = std::make_unique<AsyncCall<SearchResponse>>();
  call->lease = pool_.Acquire(shard.address);       // pooled channel
  call->context.set_deadline(deadline);             // every call is bounded
  call->reader = call->lease->PrepareAsyncSearch(&call->context, request, &queue);
  call->reader->StartCall();
  call->reader->Finish(&call->response, &call->status, call.get());
}
for (std::size_t i = 0; i < calls.size(); ++i) {    // gather
  queue.Next(&tag, &ok);
  if (ok && tag->status.ok()) responses.push_back(std::move(tag->response));
}
```

## Complexity & trade-offs

| | Cost |
|---|---|
| Latency | ~2 × (slowest shard RTT) with global scoring, ~1 × without |
| Network | `2 × shards` RPCs per uncached query |
| Data transferred | `shards × K` documents to return `K` |
| Merge | `O(shards·K)` to dedupe + `O(shards·K · log K)` for the partial sort |

**The one real cost is the second round trip.** Global scoring doubles the network hops for a
query. It is on by default because ranking that depends on placement is a correctness bug, not a
performance tuning knob — and the result cache means popular queries pay it once. ADR-0010 records
the reasoning.

## Failure modes & edge cases

- **A shard misses its deadline.** It is dropped and the query answers from the rest, reporting
  `shards_responded < shards_queried`. For search, a slightly incomplete answer now beats a
  complete one after a hung shard's TCP timeout. The caller is told, so it can decide.
- **Partial results are never cached.** Otherwise one shard's 30-second outage gets pinned into
  the cache and outlives it. This costs a cache miss during an outage — the right way round.
- **No shard answers.** The query returns empty with `shards_responded == 0` rather than an error,
  and again is not cached.
- **A document indexed on two shards** (a transient during placement changes) would appear twice;
  the merge deduplicates by `file_id`, keeping the higher score.
- **A query term no shard reported on** falls back to that shard's local `n(t)` rather than 0 —
  `n(t) = 0` maximizes IDF, which would make an unknown term the strongest signal in the query.
- **Empty term set** (a filter-only query like `author:ojas`): round 1 is skipped, since there is
  no IDF to correct.

## Alternatives we considered

- **Single round, merge local scores.** One less round trip, and what the code does with
  `global_scoring=false`. Rejected as the default: `coordinator_test` shows it produces a
  *measurably different* ranking from a single-index baseline, and "which shard is it on" is not
  something a user's result order should depend on.
- **Coordinator parses the query and ships a tree.** The parser already lives in a shared module
  (`src/common/query`) precisely so this is possible. Not done: parsing is microseconds against
  milliseconds of network, and a query string on the wire is far easier to debug than a
  serialized AST. Recorded in `search.proto` where the original co-design TODO was.
- **Thread-per-shard blocking calls.** Simpler to read, and fine at 4 shards. Rejected because it
  ties concurrency to thread count — 100 shards would mean 100 blocked threads per query. See
  [async-io](async-io.md).
- **Cache per shard instead of at the coordinator.** Complementary rather than alternative: a
  shard-level cache helps when different queries share sub-work, while the coordinator's cache
  eliminates the whole fan-out. The coordinator's is strictly cheaper per hit.

## Interview Q&A

**Q: Why can't you just sort the shards' results by score?**
Because BM25's IDF is computed from collection statistics, and each shard only has its own. The
same document scores differently depending on which shard holds it, so the scores aren't on a
common scale. You either correct the statistics first (what we do) or accept an approximate
ranking.

**Q: Why ask every shard for K results rather than K/n?**
The global top-K might all live on one shard. No shard can know how its documents rank against
another's, so only the union of full local top-Ks is guaranteed to contain the true top-K.

**Q: How do you combine the average document lengths?**
Weighted by each shard's document count: `Σ(N_i · avgdl_i) / ΣN_i`. Averaging the averages
over-weights small shards.

**Q: What happens when a shard is slow?**
Every RPC carries a deadline. On expiry the shard is dropped, the query answers from the
survivors, and the response reports how many of the queried shards actually responded. Partial
answers are not cached.

**Q: Does the extra round trip not double your latency?**
It roughly doubles *uncached* latency, yes. That is the price of a ranking that doesn't depend on
placement, and the result cache means repeat queries pay nothing — in our demo a cached query
returns in 0.01 ms against ~11 ms cold.

## References

- Elasticsearch, [Distributed search: `dfs_query_then_fetch`](https://www.elastic.co/guide/en/elasticsearch/reference/current/search-request-body.html#dfs-query-then-fetch)
  — the same two-round design and the origin of the "DFS" name.
- Jeff Dean, *Achieving Rapid Response Times in Large Online Services* (2012) — tail latency and
  partial results in fan-out systems.
- Büttcher, Clarke & Cormack, *Information Retrieval*, ch. 14 (distributed retrieval).
- [bm25](bm25.md) · [inverted-index](inverted-index.md) · [async-io](async-io.md) ·
  [lru-lfu-cache](lru-lfu-cache.md) · [ADR-0010](../architecture/adr/0010-global-scoring-dfs.md)

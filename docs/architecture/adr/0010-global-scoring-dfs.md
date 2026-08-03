# ADR-0010 — Global BM25 statistics for distributed ranking (DFS query-then-fetch)

**Status:** Accepted (2026-08-03)

## Context

Phase 3 built BM25 ranking over a single shard's index. Phase 4 must merge results from several
shards into one ranked list — and the scores are not comparable.

BM25's inverse document frequency is

```
IDF(t) = ln(1 + (N - n(t) + 0.5) / (n(t) + 0.5))
```

where `N` is the number of documents in the collection and `n(t)` the number containing `t`.
Length normalization additionally divides a document's length by the collection average `avgdl`.
All three are **collection** statistics, and under document partitioning (ADR-0006) each shard has
only its own.

The consequence is not subtle. A term appearing in 8 of shard A's 10 documents looks common there
and rare on shard B, which holds 10 documents without it. The same document, byte for byte, earns
a different score depending on which shard the ring happened to place it on. Merging those numbers
by sorting them together produces a ranking that encodes placement.

`SearchResponse.stats` has carried `doc_count` / `unique_terms` / `avg_doc_len` since the Phase 0
contract with the comment *"enables later global-stat correction"*. This ADR is that correction.

## Options considered

**1. Merge shard-local scores (the approximation).**
One round trip. Ranking is approximately right — shards usually have similar statistics when
placement is uniform, which the consistent-hashing ring makes broadly true. Deviates most exactly
where it matters: rare terms, and small or skewed shards.

**2. Two-round DFS query-then-fetch.**
Round 1 collects `N`, `n(t)` and `avgdl` from every shard; the coordinator sums them and replays
the query with the global values. Costs one extra round trip. Produces scores identical to a
single index.

**3. Maintain a replicated global dictionary.**
A shared, continuously-updated `n(t)` table so no extra round trip is needed. Removes the latency
cost, adds a distributed-consistency problem: the table must be updated on every index operation
and read consistently by every shard.

**4. Rank by a placement-independent signal instead.**
Sidesteps the problem by not using collection statistics — and throws away BM25, which is the
whole of Phase 3.

## Decision

**Two-round DFS (option 2), on by default, with option 1 available behind a flag.**

The coordinator:

1. Analyzes the query into terms (using the shared analyzer, so every shard is asked about the
   same term set).
2. Calls `SearchService.TermStats(terms)` on every shard, concurrently.
3. Sums: `N = ΣN_i`, `n(t) = Σn_i(t)`, and `avgdl = Σ(N_i · avgdl_i) / N` — **weighted by
   document count**, because `avgdl` is a per-document mean and averaging the averages would let
   a 3-document shard count as much as a 300-document one.
4. Sends those values in `SearchRequest.global_stats`; each shard ranks with global `N`, `n(t)`,
   `avgdl` but **local** term frequency and document length, which is correct — `tf` and `|d|` are
   properties of the document, not of the corpus around it.

`CoordinatorOptions::global_scoring = false` (`ATLAS_GLOBAL_SCORING=0`) selects option 1.

Contract additions to `search.proto`, all additive: a `TermStats` RPC, `TermStatsRequest`/
`TermStatsResponse`, a `GlobalStats` message, and `SearchRequest.global_stats`.

## Consequences

**Positive**

- Ranking is independent of placement. `coordinator_test` indexes one corpus two ways — split
  across four shards and into a single local engine — and asserts the distributed result matches
  the single-index result **exactly** (scores within 1e-9), on a corpus deliberately skewed so
  shard-local scoring gets it wrong. The same test asserts that with `global_scoring=false` the
  rankings *do* diverge, so the extra round trip is demonstrably buying something rather than
  performing correctness theatre.
- The correction is verifiable, not asserted. That single test is what makes this ADR checkable.
- A term no shard reported on falls back to that shard's local `n(t)` rather than 0 — `n(t)=0`
  maximizes IDF, which would make an unknown term the strongest signal in a query.

**Negative**

- **One extra round trip per uncached query**, roughly doubling uncached latency. Mitigated by the
  result cache: popular queries pay it once. Measured in the demo at ~11 ms cold, 0.01 ms warm.
- **Extra fan-out load.** `2 × shards` RPCs per uncached query instead of `shards`.
- **A slow shard now delays round 1 too.** Both rounds are deadline-bounded and a non-responding
  shard is dropped, so this degrades rather than hangs — but a straggler costs its deadline twice.
- **The statistics can be inconsistent with the search.** Rounds 1 and 2 are separate; a document
  indexed between them means the query ranks with statistics that are microseconds stale. Harmless
  for ranking, and unavoidable without a snapshot mechanism.

## Mitigations

- The result cache removes the cost entirely for repeated queries, which is where query traffic
  concentrates.
- Filter-only queries (no rankable terms) skip round 1 — nothing to correct.
- If round 1 fails on every shard, the coordinator falls back to shard-local scoring rather than
  failing the query: an approximate answer beats none.
- If the extra hop ever shows up in a profile, option 3 (a replicated dictionary) is the upgrade
  path, and the wire format already carries everything it would need.

## References

- Elasticsearch's `dfs_query_then_fetch` — the same design, and the origin of the name.
- `docs/concepts/scatter-gather.md`, `docs/concepts/bm25.md`.
- ADR-0006 (document-partitioned index), ADR-0007 (index format).

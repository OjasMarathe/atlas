# ADR-0006 — Search index sharding: partition by document (local index)

**Status:** Accepted (2026-07-21)

## Context

The inverted index must be distributed across search shards. There are two classic strategies,
and this decision shapes the entire query path. (Ojas + Harshal delegated this call to the
architecture write-up; here it is with the reasoning.)

## Options considered

### Partition by document — "local index" (chosen)
Each shard owns a **subset of documents** and builds a **complete inverted index over just those
documents**. A query is **broadcast to all shards** (scatter-gather); each returns its local
top-K; the coordinator merges to a global top-K.
- **Pros:** indexing is **local** (a new doc goes to exactly one shard → trivial incremental
  indexing); naturally balances storage; losing a shard loses only its docs; **this is what
  Elasticsearch / Solr / Lucene actually do**; composes directly with our Phase 4 scatter-gather
  coordinator; aligns with consistent-hashing storage placement (a shard indexes what its node
  stores → data locality).
- **Cons:** every query fans out to **all** shards, so latency is bounded by the slowest shard;
  global term statistics (IDF, avgdl) are per-shard, so cross-shard BM25 scores are approximate.

### Partition by term — "global index"
Each shard owns a **subset of terms** and holds the full posting list for those terms across all
documents. A query touches only the shards owning its terms.
- **Pros:** small queries touch few shards; global term stats are natural.
- **Cons:** multi-term queries must **ship large posting lists between nodes** to intersect them
  (network-heavy); **hot terms** create hotspots; indexing one document touches many shards; poor
  load balancing; phrase queries are painful. Rarely used in production for exactly these reasons.

## Decision

**Partition by document (local index per shard), co-located with the storage node that owns the
documents' chunks** — reusing the same consistent-hashing placement for data locality. Query =
coordinator scatter-gathers to all shards → each ranks with BM25 over its local docs → merge.

## Consequences

- **Positive:** simplest correct design; trivial incremental indexing; no cross-node posting-list
  shipping; matches production systems; slots straight into the [architecture](../system-architecture.md)
  read path and Phase 4.
- **Negative / accepted for M1:** query fan-out = N (all shards); **global term statistics are
  approximate** because each shard uses local IDF/avgdl. This is documented in
  [concepts/bm25.md](../../concepts/bm25.md); the fix (a small global-stats service or periodic
  stat broadcast) is a post-M1 upgrade, not an M1 requirement.
- Adds a concept note to write: **index-sharding** (document vs term partitioning).

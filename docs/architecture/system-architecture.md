# Atlas — System Architecture

How the pieces fit, the two critical data paths (write & read), the node model, and the
service boundaries. This is the map; per-module design docs live beside it as we build them.

## Components

```
                              Users / clients
                                    │
                        ┌───────────┴───────────┐
                   Web Dashboard             REST API gateway
                   (React/TS/D3)                  │
                        └───────────┬─────────────┘
                                    ▼
                          ┌───────────────────┐
                          │  Query Coordinator │   stateless; fan-out + merge
                          └─────────┬─────────┘
              ┌───────────────────┬─┴───────────────────┐
              ▼                   ▼                      ▼
   ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
   │ Metadata Service │  │  Search Service  │  │  Search Service  │  … (per shard)
   │  chunk map, ACLs │  │  inverted index  │  │  inverted index  │
   │  versions, ring  │  │  BM25 + cache    │  │  BM25 + cache    │
   └────────┬─────────┘  └────────┬─────────┘  └────────┬─────────┘
            │                     │                      │
            ▼                     ▼                      ▼
   ┌────────────────────────────────────────────────────────────┐
   │              Distributed Storage Cluster                    │
   │   Node 1        Node 2        Node 3        Node 4          │
   │   [chunks]      [chunks]      [chunks]      [chunks]        │  RocksDB + WAL
   │   each chunk replicated onto 3 distinct nodes via the ring  │
   └────────────────────────────────────────────────────────────┘
            ▲
            │
   Ingestion pipeline  ◀──  Crawler (Phase 6)  /  direct uploads
```

| Component | Responsibility | State |
|---|---|---|
| **Query Coordinator** | Accept queries, scatter to shards, gather + merge + rank top-K | Stateless (cache is advisory) |
| **Metadata Service** | Authoritative map: FileID → chunks → nodes; versions; ACLs; ring membership | Stateful; RocksDB; SPOF in M1, Raft later |
| **Search Service** | Own a shard's inverted index; tokenize, BM25-rank, cache | Stateful; index derived from stored docs |
| **Storage Node** | Store/serve/replicate chunks; checksums; WAL; heartbeat | Stateful; RocksDB |
| **Ingestion** | file → chunks → replicate → register metadata → index | Stateless worker |
| **Dashboard / API** | REST surface + live cluster/analytics view | Stateless |

## Node model

Every backend process is a **node** in the cluster and speaks gRPC. A node runs one or more
*roles* (storage, search, metadata, coordinator). For local dev, `docker-compose` starts
several single-role nodes; conceptually roles can co-locate. Cluster membership + chunk
placement are governed by the **consistent-hashing ring** (see the concept note), whose
current state the Metadata Service owns and gossips/serves to others.

## Write path (upload a document)

```
client → REST → Ingestion
  1. split file into 4 MB chunks; SHA-256 each chunk (content address)
  2. for each chunk: ring.lookup(chunkHash) → [primary, secondary, tertiary]
  3. write chunk to primary; primary replicates to secondary & tertiary (WAL first)
  4. each node acks after WAL fsync + checksum stored
  5. Ingestion registers file in Metadata: {FileID, chunkList, nodes, version, checksums}
  6. Ingestion hands extracted text to the Search Service for the owning shard → index update
```

Consistency for M1: a write acks after the primary + at least one replica are durable
(configurable quorum). Metadata write is the commit point — a file "exists" once metadata
records it.

## Read path (search a query)

```
client → REST → Query Coordinator
  1. parse query (tokens, filters, boolean/phrase)
  2. check coordinator cache (LRU/LFU) → hit? return
  3. scatter the query to all search shards in parallel (async gRPC)
  4. each shard walks its inverted index, scores docs with BM25, returns its local top-K
  5. coordinator merges the shard top-Ks, re-sorts globally, takes top-K
  6. (hydrate) fetch doc metadata/snippets from Metadata; enforce ACLs
  7. cache + return
```

Fetching an actual chunk (e.g. to show a document): Metadata → chunk→node map → read from
the nearest healthy replica, verify checksum, stream back.

## Service boundaries (gRPC / protobuf)

Contracts live in `proto/` and are the co-designed foundation (Phase 0). Sketch of the
services we expect:

- `MetadataService` — `RegisterFile`, `GetFile`, `ListVersions`, `GetRing`, `UpdateRing`.
- `StorageService` — `PutChunk`, `GetChunk`, `DeleteChunk`, `ReplicateChunk`, `Heartbeat`.
- `SearchService` — `IndexDocument`, `Search`, `Suggest` (autocomplete), `Stats`.
- `CoordinatorService` — `Query` (client-facing fan-out entry point).

Typed contracts mean the storage track and search track can be built in parallel and tested
against generated stubs / mocks.

## Consistency & failure model (M1)

- **Replication factor 3**, placement by ring walk to 3 distinct physical nodes.
- **Durability:** WAL fsync before ack; checksums verified on every read.
- **Availability:** node death detected by heartbeat timeout → replica promotion →
  re-replication to restore factor 3. Reads/searches route around dead nodes.
- **Metadata** is the M1 single-point-of-failure (documented tradeoff); Raft-replicating it
  is the Phase 2 stretch / an early post-M1 upgrade.
- **CAP stance:** within one cluster we favor consistency of the metadata/commit point and
  availability of reads via replicas; we are not solving cross-partition writes for M1.

## Intended code layout

```
proto/            service contracts (source of truth for boundaries)
src/
  common/         config, logging, hashing, checksums, thread pool, gRPC helpers
  metadata/       Metadata Service
  storage/        Storage Node (chunks, WAL, replication, heartbeat)
  search/         Search Service (tokenizer, inverted index, BM25, cache)
  coordinator/    Query Coordinator (scatter-gather, cache)
  ingestion/      pipeline: chunk → replicate → register → index
tests/            unit + integration (multi-node) tests
deploy/           Dockerfiles, docker-compose, k8s manifests, CI
dashboard/        React/TS frontend (later)
```

Cross-cutting concepts (thread pool, caches, hashing, WAL) live in `src/common/` and each
gets a note in [../concepts/](../concepts/).

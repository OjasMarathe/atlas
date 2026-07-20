# Atlas — Roadmap

Phase-by-phase, not day-by-day (some days we grind, some days we don't). Each phase has
a **Definition of Done (DoD)** — it isn't finished until every box is checked *and its
concepts are documented*.

- **Milestone 1 (target: 2026-07-30):** Phases 0–4 — a working, demoable distributed core.
- **Post-M1:** Phases 5–9 — the depth we keep adding for the resume.

> **C++ reality check.** Phases 0–4 in C++ in ~10 days (part-time, two people) is
> aggressive. Priority order if we fall behind by ~Jul 27:
> **must-have demo core = Phases 0 + 1 + 2 + minimal keyword search (3a).**
> Distributed query (Phase 4) may slip past the 30th and that's fine — a fault-tolerant
> distributed *store* with basic search is already a compelling demo.

---

## Phase 0 — Foundations `M1`

Scaffolding both people build against. **Co-designed together** (see collaboration doc).

- Build system (CMake + a dependency manager: vcpkg or Conan), formatting (clang-format),
  static analysis (clang-tidy), sanitizers wired (ASan/TSan/UBSan).
- **`proto/` service contracts** — the gRPC interfaces for Metadata, Storage, Search,
  Coordinator. This is the single most important artifact; it lets us work in parallel.
- A single-node "hello cluster" skeleton: a node process that starts, serves a gRPC
  health check, and logs. Docker + docker-compose to run N of them locally.
- GitHub Actions CI: build + test on every PR.

**DoD:** `docker compose up` starts a 3-node cluster; CI is green on a PR; `proto/` compiles
and generates C++ stubs; a health-check RPC round-trips between nodes.

## Phase 1 — Distributed File System `M1`

The heart. Store files as replicated chunks; never scan disks for metadata.

- **Chunking** — split files into fixed 4 MB chunks; content-addressed by SHA-256.
- **Metadata service** — per file: FileID, owner, permissions, chunk list, replication
  factor, checksums, compression, encryption-key-id, ctime, version, mtime. Stored in
  RocksDB, separate from chunk data.
- **Storage engine** — chunk put/get/delete on each node backed by RocksDB; **WAL** for
  crash consistency; per-chunk **checksums** verified on read.
- **Consistent hashing ring** — implemented by hand, with virtual nodes; nodes join/leave
  and only a fraction of data migrates.
- **Replication** — each chunk on primary + secondary + tertiary (3 distinct nodes chosen
  by walking the ring).
- **Versioning** — every edit creates a new version; old versions retained until GC.

**DoD:** upload a file → it's chunked, checksummed, versioned, and each chunk exists on 3
nodes; reads verify checksums; adding a 4th node migrates only its share of chunks.
**Concepts owed:** [chunking], [consistent-hashing] ✅, [replication], [wal], [sha256-checksums], [versioning].

## Phase 2 — Fault Tolerance `M1`

Make it survive death.

- **Heartbeats** between nodes + the coordinator; configurable timeout.
- **Failure detection** — missed heartbeats mark a node dead.
- **Replica promotion** — if a chunk's primary dies, a replica takes over.
- **Self-healing** — under-replicated chunks are re-replicated onto healthy nodes to
  restore the replication factor.
- *(Stretch)* **Raft** for metadata-service leader election / replicated log, instead of a
  single metadata node being a SPOF.

**DoD:** kill a node during traffic → no data loss, cluster detects it within the timeout,
re-replicates to full factor, and search/read keep working.
**Concepts owed:** [heartbeat-failure-detection], [replica-promotion], [self-healing], [raft] (if attempted).

## Phase 3 — Search Engine `M1`

Turn stored bytes into answers. *(3a = the minimal must-have subset.)*

- **3a (must-have):** ingestion pipeline (clean → extract text → tokenize → normalize),
  **inverted index** with compressed posting lists, **BM25** ranking, boolean queries.
- **3b:** Trie **autocomplete**, **BK-tree + Levenshtein** spell correction, phrase search,
  filters (author/date/type/lang), **incremental indexing**.

**DoD (3a):** index the demo corpus; a keyword query returns BM25-ranked results with the
right documents on top, sub-second.
**Concepts owed:** [tokenization-stemming], [inverted-index], [posting-list-compression], [bm25] ✅, [trie-autocomplete], [bk-tree-levenshtein], [boolean-phrase-search].

## Phase 4 — Distributed Query Engine `M1`

Make search itself distributed.

- **Coordinator** splits a query, **scatter-gathers** across search shards, **merges** and
  sorts to return top-K.
- **Caching** — implement **LRU** and **LFU**, compare hit ratios; optional Redis comparison.
- **Connection pool + thread pool + async networking** (Boost.Asio) for concurrency.

**DoD:** query hits ≥3 shards in parallel, results merged correctly, warm cache measurably
faster; sustains N concurrent clients in a load test.
**Concepts owed:** [scatter-gather], [lru-lfu-cache], [thread-pool], [connection-pool], [async-io].

---

## Post-Milestone-1 phases

## Phase 5 — Semantic Search
Sentence-transformer **embeddings** (Python service) → **vector store** → **cosine
similarity** → **hybrid ranking** (0.6·BM25 + 0.4·vector). Query by meaning, not keywords.
**Concepts:** [embeddings], [vector-search], [hybrid-ranking].

## Phase 6 — Distributed Web Crawler
Thread pool, frontier queue, **robots.txt**, rate limiting, **Bloom-filter** dedup, priority
queue, retry with backoff, checkpoint recovery; parser extracts title/author/links/tables.
Populates Atlas from Wikipedia/arXiv/GitHub/docs.
**Concepts:** [crawler-architecture], [bloom-filter], [rate-limiting], [robots-politeness].

## Phase 7 — Security
**JWT** auth, **RBAC**, per-document **ACLs**, **encrypted chunks**, **TLS**, audit logs,
API keys, rate limiting.
**Concepts:** [jwt-auth], [rbac-acl], [encryption-at-rest], [tls].

## Phase 8 — Observability & Analytics
**Prometheus** metrics, **Grafana** dashboards, **OpenTelemetry** distributed tracing,
structured logging, health checks, alerting. React/D3 analytics: QPS, latency, cache-hit
ratio, node utilization, replication lag, storage growth.
**Concepts:** [metrics-prometheus], [distributed-tracing], [analytics-dashboard].

## Phase 9 — Infrastructure & Hardening
Dockerize every service, **Kubernetes** manifests, CI/CD via GitHub Actions, **load
testing**, **benchmarking**, **chaos testing** (kill nodes, partition the network).
**Concepts:** [containerization], [k8s-orchestration], [chaos-testing], [load-testing].

---

## Phase dependency graph

```
0 ──▶ 1 ──▶ 2
      │      
      └──▶ 3 ──▶ 4        (3 needs stored docs from 1; 4 needs 3's index)
                 │
                 └──▶ 5   (hybrid ranking builds on BM25)
6 (crawler) feeds 1/3 but is independent — can be built anytime after 0
7, 8, 9 layer over everything and can start in parallel once 1–4 stabilize
```

Legend: `M1` = required for the July-30 Milestone-1 demo. ✅ = concept note already written.

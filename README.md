# Atlas

**A distributed enterprise knowledge platform, built from scratch.**

Atlas ingests an organization's scattered documents — PDFs, wikis, code, research
papers — stores them **chunked and replicated across a cluster of nodes** (GFS-style),
and lets you **search the entire knowledge base** with real ranking (BM25 today,
hybrid semantic later). It survives node failures, rebalances automatically, and
exposes it all through a REST API and dashboard.

> The goal is **not** to replace Elasticsearch or Google Drive. It is to *implement
> the internals ourselves* — consistent hashing, replication, failure recovery,
> inverted indexes, ranking, consensus — and understand every one of them deeply.

---

## The demo we're building toward (Milestone 1)

```
1. Upload "OperatingSystems.pdf"
      → split into 4 MB chunks
      → each chunk replicated onto 3 distinct nodes (consistent hashing)
      → metadata (chunk map, checksums, versions) recorded separately
2. Search "how does a filesystem journal writes?"
      → coordinator fans the query out across shards
      → each node searches its inverted index, ranks with BM25
      → results merged, top-K returned
3. Kill a storage node mid-demo
      → heartbeat timeout detected
      → replicas promoted, under-replicated chunks re-replicated
      → search still returns correct results
```

That single end-to-end demo — *store distributed, search distributed, survive failure* — is Milestone 1.

---

## Architecture (target)

```
                          Users
                            │
                 ┌──────────┴──────────┐
            Web Dashboard           REST API
            (React/TS/D3)               │
                            Query Coordinator
                                    │
                  ┌─────────────────┴─────────────────┐
            Metadata Service                     Search Service
         (chunk map, ACLs,                    (inverted index,
          versions, ring state)                BM25, cache)
                  │                                   │
                  └─────────────────┬─────────────────┘
                                    │
                    Distributed Storage Cluster
              ┌────────┬────────┬────────┬────────┐
              │ Node 1 │ Node 2 │ Node 3 │ Node 4 │   ← chunks, replicated 3×
              └────────┴────────┴────────┴────────┘
                                    ▲
                                    │
                     Ingestion  +  Crawler
```

Full detail: [docs/architecture/system-architecture.md](docs/architecture/system-architecture.md).

---

## Tech stack

| Layer | Choice | Why |
|---|---|---|
| Core services | **C++20** | Systems-level control; the concurrency primitives *are* the learning |
| RPC / contracts | **gRPC + Protocol Buffers** | Typed service boundaries between coordinator, metadata, storage |
| Networking | **Boost.Asio** | Async I/O for the query/connection layer |
| Storage engine | **RocksDB** (embedded LSM) | Don't hand-roll a KV store; focus on the *distributed* layer above it |
| Semantic module | **Python** (later phase) | Best ecosystem for embeddings / sentence-transformers |
| Dashboard + API | **React, TypeScript, Tailwind, D3.js** | Analytics + live cluster view |
| Infra | **Docker, Docker Compose, Kubernetes, GitHub Actions** | Reproducible multi-node cluster + CI |

The C++ decision is recorded as [ADR-0001](docs/architecture/adr/0001-language-cpp20.md).

---

## Repository layout

```
atlas/
├── README.md                 ← you are here
├── docs/                     ← the brain of the project (read this first)
│   ├── 00-overview.md        ← vision, goals, non-goals, success criteria
│   ├── plan/                 ← roadmap, collaboration workflow, progress log
│   ├── architecture/         ← system design + Architecture Decision Records
│   └── concepts/             ← ONE deep note per concept we learn (our core goal)
├── proto/                    ← gRPC/protobuf service contracts (co-designed first)
├── src/                      ← C++ services (added in Phase 0)
├── dashboard/                ← React/TS frontend (later phase)
└── deploy/                   ← Docker / k8s / CI (later phase)
```

---

## Status

**Phases 0–2 complete** — a running, self-healing distributed file system: content-addressed
chunking, consistent-hashing placement, 3× replication with a 2-of-3 write quorum, copy-on-write
versioning, failure detection, and automatic re-replication. (Phase 3's search engine is on its own
branch.) See [docs/plan/roadmap.md](docs/plan/roadmap.md) for the phased plan and
[docs/plan/progress.md](docs/plan/progress.md) for the running log.

## Team

- **Ojas Marathe** — [@ojas](https://github.com/)
- **Harshal** — [@harshal](https://github.com/)

Workflow: [docs/plan/collaboration.md](docs/plan/collaboration.md).

## Building & running

**Prerequisites:** a C++20 compiler, CMake ≥ 3.24, Ninja, Docker, and
[vcpkg](https://github.com/microsoft/vcpkg). gRPC / Protobuf / GoogleTest are pulled
automatically by vcpkg from [`vcpkg.json`](vcpkg.json).

```bash
# one-time: install vcpkg and point VCPKG_ROOT at it
git clone --depth 1 https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg            # add this to your shell profile

# configure + build (first run is slow: vcpkg compiles gRPC, then caches)
cmake --preset default
cmake --build build

# run the 3-node cluster natively and watch the heartbeats
./scripts/run-cluster-local.sh

# ...or in containers
docker compose up --build
```

## Running a cluster

`atlas_node` runs as either role, selected by `ATLAS_ROLE`:

- **`storage`** (default) — serves `StorageService` over a local RocksDB chunk store, and registers
  itself into the ring at startup if `ATLAS_METADATA` is set.
- **`metadata`** — serves `MetadataService` (file map + ring) and runs the **maintenance loop**:
  probe every member for liveness, then re-replicate any chunk that has fallen below the
  replication factor.

```bash
docker compose up --build            # 1 metadata + 4 storage nodes

./build/atlas nodes                  # cluster membership
./build/atlas put report.pdf ./report.pdf
./build/atlas info report.pdf        # chunks + which nodes hold them
./build/atlas get report.pdf ./out.pdf
```

`atlas` talks to `ATLAS_METADATA` (default `127.0.0.1:50050`).

### See it heal itself

```bash
./scripts/demo-self-healing.sh
```

Starts a real 5-process cluster, uploads a file, **kills one of the nodes holding it**, and shows
the control plane detect the failure and re-replicate the chunk onto a healthy node — then
downloads the file and verifies it byte-for-byte:

```
uploaded demo.txt (266669 bytes, 1 chunk(s), replicated 3x)
  87be32921eb6…  holders: node4 node1 node2
== killing node2 ==
[metadata] nodes down: node2
[metadata] healed 1 replica(s) across 1 under-replicated chunk(s)
  87be32921eb6…  holders: node4 node1 node2 node3      ← restored
OK — downloaded file is identical to the original
```

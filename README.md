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

**Phase 0 — Foundations** (in progress). See [docs/plan/roadmap.md](docs/plan/roadmap.md)
for the full phased plan and [docs/plan/progress.md](docs/plan/progress.md) for the running log.

## Team

- **Ojas Marathe** — [@ojas](https://github.com/)
- **Harshal** — [@harshal](https://github.com/)

Workflow: [docs/plan/collaboration.md](docs/plan/collaboration.md).

## Building & running

**Prerequisites:** a C++20 compiler, CMake ≥ 3.24, Ninja, Docker, `pkg-config`, and
[vcpkg](https://github.com/microsoft/vcpkg). gRPC / Protobuf / GoogleTest are pulled
automatically by vcpkg from [`vcpkg.json`](vcpkg.json).
On macOS: `brew install cmake ninja pkg-config autoconf automake clang-format`.

```bash
# one-time: install vcpkg at the baseline pinned in vcpkg.json
#   NOTE: a full clone, not --depth 1 — a shallow clone of vcpkg's tip does NOT contain the
#   pinned "builtin-baseline" commit, and Configure then fails to resolve dependencies.
git clone https://github.com/microsoft/vcpkg ~/vcpkg
git -C ~/vcpkg checkout "$(jq -r '."builtin-baseline"' vcpkg.json)"
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg            # add this to your shell profile

# configure + build + test (first run is slow: vcpkg compiles gRPC & OpenSSL from source,
# ~30-60 min, then caches to ~/.cache/vcpkg so later builds are fast)
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure

# run the 3-node cluster natively and watch the heartbeats
./scripts/run-cluster-local.sh

# ...or in containers
docker compose up --build
```

**Sanitizers:** `cmake --preset default -DATLAS_SANITIZE=undefined` (see
[`CMakeLists.txt`](CMakeLists.txt)). AddressSanitizer is known to hang before `main()` on
macOS 26 / Apple clang 17 — even for a trivial program — so prefer `undefined` locally and let
Linux CI or the Docker image exercise `address`.

Phase 0's `atlas_node` is a skeleton: it serves `StorageService.Heartbeat` and pings its peers,
proving the toolchain, proto codegen, gRPC networking, and multi-node wiring end to end. Real
storage/search behavior arrives in Phases 1–4.

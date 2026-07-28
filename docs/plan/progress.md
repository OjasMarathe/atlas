# Atlas — Progress Log

Running changelog so either of us can resume the other's thread cold. Newest first.
One entry per meaningful landing (merged PR, phase milestone, decision). Keep it terse.

Format: `YYYY-MM-DD — [phase] what changed — who`

---

## 2026-07

- **2026-07-28** — [Phase 1] Verified + landed: **#4 consistent-hashing ring** (`src/common/hash_ring`,
  13/13 tests incl. the minimal-movement property) and **#2 storage layer 1** — RocksDB-backed
  `ChunkStore` (`src/storage/chunk_store`, checksum-on-write/read + crash-durable, verified vs real
  RocksDB 11.x; caught the 11.x `DB::Open` signature change). Phase 0 heartbeat demo ran (3 nodes +
  failure detection); node log-flush fix. Concept notes: wal, chunking, sha256-checksums;
  consistent-hashing marked implemented. Next: gRPC StorageService wrapper. — Ojas

- **2026-07-27** — [Phase 1] Phase 0 PR #1 merged to `main`. Started Phase 1 (DFS) on
  `phase-1/dfs`: ADR-0004 (2-of-3 replication, W=2/R=1) + `module-01-dfs` design. **Piece #1 done
  + verified** — from-scratch SHA-256 (`src/common/sha256`, matches FIPS vectors) + chunking
  (`src/dfs/chunking`), 16/16 tests passing (built dependency-free via clang++ to sidestep the slow
  gRPC build). Concept notes: chunking, sha256-checksums. gRPC vcpkg build still caching (needed for
  the storage-node pieces + the heartbeat demo). — Ojas

- **2026-07-21** — [Phase 0] Decisions locked & recorded as ADRs: **vcpkg** (0003), **single
  metadata node for M1** (0005), **document-partitioned search index** (0006), **RocksDB** engine
  (0002). Drafted `proto/` v0 contracts (common, storage, metadata, search, coordinator) —
  awaiting joint review. Toolchain probe: have cmake 4.1 / ninja / Apple clang 15 (C++20) /
  docker / brew; **missing vcpkg, gRPC, gh**. Repo pushed to github.com/OjasMarathe/atlas. — Ojas

- **2026-07-20** — [Phase 0] Repo initialized (`main`), docs scaffolded: overview, roadmap,
  collaboration model, system architecture, ADR-0001 (C++20), concepts hub + template +
  two flagship notes (consistent-hashing, BM25). Decisions locked: **C++20 core**,
  **Milestone 1 = distributed core demo by Jul 30**, **hybrid co-design + PR workflow**. — Ojas

---

## Backlog / next up

- [x] **Phase 0** — dependency manager + engine + metadata + sharding decisions → ADRs 0002/0003/0005/0006.
- [x] **Phase 0** — draft `proto/` v0 contracts (common, storage, metadata, search, coordinator).
- [ ] **Phase 0** — review `proto/` v0 together (Harshal) → merge.
- [ ] **Phase 0** — bootstrap vcpkg + `vcpkg.json` manifest (grpc, protobuf, rocksdb, boost, gtest).
- [ ] **Phase 0** — CMake wiring (proto/grpc codegen) → compiles on both machines.
- [ ] **Phase 0** — single-node gRPC health-check skeleton + docker-compose 3-node cluster.
- [ ] **Phase 0** — GitHub Actions CI (build + test, vcpkg binary cache).
- [ ] install `gh` on both machines (`brew install gh`) for the PR workflow.
- [ ] **ADR-0004** — replication & consistency (ack semantics) — Phase 1.
- [ ] **ADR-0007** — inverted-index format & posting-list compression — Phase 3.

## Resolved decisions

- Deps: **vcpkg** (0003) · Metadata: **single node for M1** (0005) · Search sharding:
  **partition by document** (0006) · Engine: **RocksDB** (0002) · Language: **C++20** (0001).

## Open questions to resolve together

- Boolean/phrase/filter parsing — in the search shard or the coordinator? (`search.proto` TODO)
- Expose per-node free/used bytes in heartbeats for placement/balancing? (`storage.proto` TODO)
- Replication ack semantics for Phase 1 (primary + how many replicas before we ack)? → ADR-0004

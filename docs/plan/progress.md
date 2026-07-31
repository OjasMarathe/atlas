# Atlas — Progress Log

Running changelog so either of us can resume the other's thread cold. Newest first.
One entry per meaningful landing (merged PR, phase milestone, decision). Keep it terse.

Format: `YYYY-MM-DD — [phase] what changed — who`

---

## 2026-08

- **2026-08-01** — [Phase 2] **Atlas is now a runnable, self-healing cluster** (not just a test
  harness). 11/11 tests green.
  - **Node roles** — `ATLAS_ROLE=storage|metadata` in one binary. Storage nodes **self-register**
    into the ring at startup (retrying, since the control plane may still be coming up), advertising
    `ATLAS_ADVERTISE` because `0.0.0.0` isn't routable from a peer.
  - **Maintenance loop** (`src/cluster/maintenance`) — the metadata node now probes + heals on a
    timer by itself; `RunOnce()` stays synchronous so tests drive a round directly. Logs liveness
    *transitions* only, so a steady dead node isn't reprinted every round. Needed
    `MetadataServiceImpl::SnapshotRing()` for a consistent membership view off the RPC threads.
  - **`atlas` CLI** — `put` / `get` / `info` / `nodes`. `info` prints each chunk's holders, which is
    what makes healing visible from the outside.
  - **`docker-compose.yml`** rewritten: 1 metadata + 4 storage, auto-registering. Closes the
    "runnable demo" gap Harshal raised in the PR #5 review.
  - **`scripts/demo-self-healing.sh`** — real 5-process demo: upload → `kill -9` a holder →
    `[metadata] nodes down: node2` → `healed 1 replica(s)` → placement shows a fresh holder →
    download verified byte-identical. **Ran it; it works.**
  - **`maintenance_test`** covers the loop in CI (the demo script isn't run there): one round with
    `failure_threshold=1` both detects the death and restores the factor, and converges after.
  - **Still open:** node-join chunk migration (Phase 1 DoD leftover), GC of surplus chunks, Raft
    (stretch). — Ojas

## 2026-07

- **2026-07-29** — [Phase 2] **Self-healing works — the cluster restores its own replication factor.**
  Branch `phase-2/fault-tolerance` off the merged `main`. Three slices, 10/10 tests green:
  - **Failure detection** (`src/cluster/health_tracker`, `src/cluster/prober`) — **pull-based**: the
    control plane probes each node via the existing `StorageService.Heartbeat`, declaring death only
    after N *consecutive* misses (one success revives). No proto change. `ProbeOnce()` is
    synchronous so tests assert exactly when a node is declared dead instead of sleeping.
  - **Live chunk-location index** (`c/<chunk_id>` in the metadata store) — mutable holder set kept
    out of the immutable version blobs and written in the same atomic batch; `GetFile` serves it, so
    readers see healed placements with **zero client change**. Also extracted
    `src/storage/chunk_transfer` so client + healer share one streaming implementation.
  - **Healer** (`src/cluster/healer`) — `RepairOnce` finds chunks below R live holders, pulls from a
    survivor, pushes onto a fresh ring-chosen live node, records it. Idempotent/convergent.
  - **`self_healing_test`**: upload → kill a holder → 2 missed probes → dead → exactly 1 repair onto
    a node outside the original three (verified it really serves the bytes) → back to 3 live holders
    → second pass is a no-op → file still downloads.
  - Notes: heartbeat-failure-detection, self-healing (+ why **replica promotion is degenerate** in
    Atlas: immutable chunks have no write-owning primary). **ADR-0009** records the Phase 2 model
    (0008 left reserved for the search track's query-parsing ADR).
  - Fixed a now-wrong e2e assertion: re-uploading identical bytes hits the same chunk id, so the
    location index rightly still lists a dead node as a holder (its bytes are unreachable, not
    gone) — the partial-write regression test now targets a *fresh* chunk mapped onto the dead node.
  - **Still open in Phase 2:** node-join chunk migration (the Phase 1 DoD leftover), wiring the
    probe/heal loop into `atlas_node`, GC of surplus/dead chunks, Raft (stretch). — Ojas

- **2026-07-29** — [Phase 1] **Review fixes (PR #5, Harshal).** Two real defects found and fixed:
  (1) `MetadataStore::RegisterFile` wrote the version blob and the `latest` pointer as two separate
  `Put`s — **not crash-atomic at the system's commit point**; now one `rocksdb::WriteBatch(sync)`.
  (2) `AtlasClient::Upload` recorded **every intended** replica in the placement even when a write
  failed, so an under-replicated chunk looked healthy forever — and Phase 2's healer reads exactly
  that list; now only **acked** nodes are recorded (intended-vs-actual = the healer's input).
  Test gaps closed: client-path **multi-chunk** upload/download (ordering + boundaries, small
  chunk_size + a real >4 MiB file) and a regression test that a write with a dead replica records
  2 holders, not 3. `AtlasClient` takes a configurable chunk_size for testability. 8/8 green. — Ojas

- **2026-07-29** — [Phase 1] **DFS built — Milestone-1 DoD met except node-join migration.** Control + data
  plane: **MetadataService** (`src/metadata/metadata_service`, gRPC over the versioned store + the
  hash ring; join/leave membership) and the **ingestion client** (`src/client/`) — Upload chunks a
  file, places each chunk on 3 ring-chosen nodes, acks at **W=2**, commits to metadata; Download
  reads back with **read-around** over healthy replicas + whole-file checksum. **End-to-end test**
  (`phase1_e2e_test`: real metadata + 4 storage nodes) proves upload → chunk on 3 distinct nodes →
  download → **kill a replica → still served via read-around** → re-upload → versioning. **8/8 tests
  green.** Concept note: replication. **Not** done, so the roadmap's Phase-1 DoD is met *except* its
  last clause: **node-join chunk migration** (the ring's minimal-movement property is unit-tested,
  but no physical chunk move happens). Left to Phase 2 (same machinery as self-healing): that
  migration + **re-replication** of a lost copy, and GC of dead chunks. Also pending: reconcile
  `main.cpp`/CMake/docs with Harshal's Phase-3 PR #4 at merge. — Ojas

- **2026-07-28** — [Phase 1] Verified + landed: **#4 consistent-hashing ring** (`src/common/hash_ring`,
  13/13 tests incl. the minimal-movement property) and **#2 storage layer 1** — RocksDB-backed
  `ChunkStore` (`src/storage/chunk_store`, checksum-on-write/read + crash-durable, verified vs real
  RocksDB 11.x; caught the 11.x `DB::Open` signature change). Phase 0 heartbeat demo ran (3 nodes +
  failure detection); node log-flush fix. Concept notes: wal, chunking, sha256-checksums;
  consistent-hashing marked implemented. **#2 layer 2 done + verified**: gRPC StorageService over the ChunkStore (put/get/delete/replicate/
  heartbeat) + loopback integration test, wired into atlas_node (a real storage node now); 5/5 tests
  green. **#3 metadata store done + verified**: versioned `MetadataStore`
  (`src/metadata/metadata_store`, copy-on-write versioning + persistence, 6/6 tests). Next:
  MetadataService gRPC wrapper + ring mgmt, then replication (#5). — Ojas

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
- [ ] **Runnable demo** (PR #5 review): `atlas_node` currently registers only the storage role, so
  `docker compose up` gives storage nodes but no metadata service, and there's no client binary —
  the M1 demo runs only in-process inside `phase1_e2e_test`. Needs an `ATLAS_ROLE=metadata|storage`
  switch + a small `atlas put/get` CLI + a compose file with a metadata service. **Before the demo.**
- [ ] **Phase 1 leftover** — node-join chunk migration (ring property proven; physical move deferred
  to Phase 2, which shares the re-replication machinery).
- [ ] **ADR-0007** — inverted-index format & posting-list compression — Phase 3.

## Resolved decisions

- Deps: **vcpkg** (0003) · Metadata: **single node for M1** (0005) · Search sharding:
  **partition by document** (0006) · Engine: **RocksDB** (0002) · Language: **C++20** (0001).

## Open questions to resolve together

- Boolean/phrase/filter parsing — in the search shard or the coordinator? (`search.proto` TODO)
- Expose per-node free/used bytes in heartbeats for placement/balancing? (`storage.proto` TODO)
- Replication ack semantics for Phase 1 (primary + how many replicas before we ack)? → ADR-0004

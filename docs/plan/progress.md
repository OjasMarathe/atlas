# Atlas — Progress Log

Running changelog so either of us can resume the other's thread cold. Newest first.
One entry per meaningful landing (merged PR, phase milestone, decision). Keep it terse.

Format: `YYYY-MM-DD — [phase] what changed — who`

---

## 2026-08

- **2026-08-01** — [Phase 2] **PR #7 merged with `main` (now Phase 0+1+3) and reviewed.** 13/13
  test targets green, clang-format clean.
  - **Conflicts resolved:** `CMakeLists.txt` (one `atlas_node` links both tracks — storage +
    search shard *and* metadata service + maintenance, since `ATLAS_ROLE` picks the role at
    runtime); `src/node/main.cpp` (body auto-merged; header comment rewritten for both roles —
    `ATLAS_PEERS` is gone, the Phase-0 peer ping is superseded by the real prober); the ADR index
    (each side listed the other's ADR as "upcoming" — now 0007 *and* 0009 are indexed, only 0008
    outstanding, plus a duplicated 0004 row dropped); `README.md`; `progress.md`.
  - **Review fix — a real lost-update race.** Phase 2 gave the metadata node a *second* writer:
    the maintenance loop's healer holds a raw `MetadataStore*` and never goes through
    `MetadataService`, so the service's mutex protected nothing against it. `AddChunkLocation`,
    `RemoveChunkLocation` and `RegisterFile`'s holder merge are all read-modify-write on the same
    `c/<chunk_id>` key, and RocksDB's per-operation atomicity does not make a RMW atomic. A
    re-upload racing a repair could drop a holder from the index — leaving a replica that exists
    on disk invisible to readers *and* to the healer, which would then re-copy the bytes every
    round forever. `MetadataStore` is now internally thread-safe (`std::shared_mutex`; shared for
    reads, exclusive for the RMW paths), and the service's mutex was narrowed to the
    ring/membership it actually owns. Same fix closes a second hole: concurrent `RegisterFile`s
    for one file could both pick the same next version.
  - **Review fix — `docker compose up --build` was broken.** `CMakeLists` declares
    `tools/search_demo.cpp` and `tools/atlas_cli.cpp`, but the Dockerfile never copied `tools/`,
    and CMake validates every declared source path at configure time. CI never caught it because
    CI doesn't build the image. Also, `docker-compose.yml` documents
    `docker compose exec metadata atlas nodes`, but the `atlas` CLI was never built into or
    installed in the image — both fixed.
  - **Review fix — `HealthTracker::Forget` had a unit test and no caller.** A node that left the
    ring while failing kept its failure count forever (nothing probes it, so nothing can reset
    it), so `DeadNodes()` reported a cleanly-departed node as permanently down and the map grew
    without bound. Added `Retain(members)`, called once per maintenance round, + a test.
  - `scripts/run-cluster-local.sh` still set the removed `ATLAS_PEERS` and started no metadata
    node, so nothing registered into the ring — rewritten for the role model (1 metadata + 3
    self-registering storage). — Harshal

- **2026-08-01** — [Phase 2] **Node-join migration — Phases 1 and 2 are now DoD-complete.** 12/12
  tests green.
  - **`Healer::RebalanceOnce`** — the other half of placement maintenance. Repair reacts to
    *missing* copies; a node join creates none (every chunk still has R live holders), so the
    newcomer would stay empty forever. Rebalance reacts to *misplacement*: the ring wants this
    chunk somewhere it isn't. Runs after repair in the maintenance loop, so durability always
    precedes tidiness.
  - **Safety rules, deliberately:** copy-then-evict (never open a durability hole for a placement
    preference), skip already-degraded chunks entirely, and evict only holders outside the ideal
    set so the count lands back at exactly R.
  - **`rebalance_test` proves the roadmap's Phase-1 DoD clause** — "adding a node migrates only its
    share" — at the *data* level, not just the ring level: 5→6 nodes, 40 chunks,
    **`21/120 replicas moved (17.5%)`** vs the ideal 1/6 ≈ 16.7%. Every move landed on the newcomer
    (zero churn between existing nodes), every chunk kept exactly 3 holders throughout, and the file
    downloaded byte-identical afterwards. `hash % N` placement would have moved nearly all 120.
  - `RunOnce()` now returns `MaintenanceReport{heal, rebalance}`; added `DeleteChunk` to
    `chunk_transfer`. Concept note: **chunk-migration**.
  - **Remaining (optional):** GC of surplus chunks, repair/migration rate limiting, Raft (stretch).
    — Ojas

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

- **2026-08-01** — [Phase 3] **PR #4 merged with `main` (Phase 0+1) and reviewed.** `main` had moved
  on (Phase 1 landed first), so the search branch carried the conflicts:
  - **Conflicts resolved:** `src/node/main.cpp` — one node now registers **both** `StorageService`
    (RocksDB chunk store) and its own `SearchService` shard on one port; `CMakeLists.txt` rewritten
    as libraries → executables → tests, unioning both tracks; `docs/plan/progress.md` re-interleaved
    in date order. ADR index was missing 0004 (auto-merge kept the older list) — restored.
  - **Verified together:** clean build, **9/9 test targets green** (138 search cases + the storage
    and Phase-1 e2e suites), tree-wide `clang-format` clean, `atlas_node` starts and listens with
    both services, `atlas_search_demo` sub-ms over the real 16-doc concepts corpus (which now spans
    both tracks' notes — a nice cross-check).
  - **Review fixes applied:** phrase matching resolved each term's posting *inside* the position
    loop though the lookup is anchor-invariant — hoisted to once per document; `DecodePostingList`
    and `InvertedIndex::Load` now sanity-check declared counts against the remaining buffer, so a
    truncated/corrupt index returns `false` instead of `reserve()`-ing an arbitrary amount; CI
    lint/tidy globs widened from the search dirs to the whole tree (they were silently skipping all
    the storage/metadata/client code).
  - Harshal's earlier "fixed phase 3a issues" commit had already addressed all three points from
    the 3a review (single gRPC error channel, multi-token query words expanded rather than dropped,
    bare-`NOT` doc matching the code). — Ojas

## 2026-07

- **2026-07-31** — [Phase 3] **3b complete.** Phrase search, filters, incremental indexing,
  autocomplete, spell correction, and posting-list compression. **138 tests** green (from 76),
  also green under `-DATLAS_SANITIZE=undefined`.
  - **Incremental indexing** (`inverted_index.*`) — re-indexing a `file_id` now supersedes
    instead of silently duplicating (the Phase 3a gap). Deletes **tombstone**; `Compact()`
    reclaims and renumbers. BM25 statistics (`N`, `avgdl`, `n(t)`) count live documents only, or
    every score skews. → note `incremental-indexing`.
  - **Phrase search** — `"write ahead log"` matches only adjacent terms, via an anchor position
    aligning every term at `p + its phrase offset`. Vindicates the Phase 3a decision to record
    positions pre-filter. **Subtlety found:** stop words aren't indexed, so a phrase can only
    check gap *width*, not contents — `"chunks of the ring"` also matches "chunks on a ring".
    Lucene behaves identically; documented rather than hidden.
  - **Field filters** — `author:ojas AND type:note`. Filters select but never rank
    (`PositiveTerms` skips them), so a filter-only query returns its set unranked. A bare `NOT`
    still returns nothing — `HasPositiveFilter` distinguishes "selects a set" from "excludes".
  - **Trie autocomplete** (`trie.*`) + the `Suggest` RPC, previously UNIMPLEMENTED. Ranked by
    corpus frequency. Fed **surface forms**, so it offers "replication", not the stem "replic" —
    `Analyze()` now carries both. → note `trie-autocomplete`.
  - **BK-tree + Levenshtein** (`bk_tree.*`) — `hashign`→`hashing`, `replicaton`→`replication`.
    Pruning is property-tested against an exhaustive scan, since it must not change the answer.
    → note `bk-tree-levenshtein`.
  - **Posting-list compression** (`posting_codec.*`) — delta + varint per ADR-0007, plus whole
    index `Serialize()`/`Load()`. Measured on `docs/`: 107.5 KiB of postings → 70.9 KiB whole
    snapshot (**1.52×**) in ~1 ms. → note `posting-list-compression`.
  - **Known gaps:** compaction is manual (no tombstone-ratio trigger); the vocabulary isn't
    decremented on delete, so a stale suggestion is possible; `ScoredDoc.snippet` is still empty
    (needs byte offsets from the tokenizer); the index is still in memory — RocksDB persistence
    is the remaining half of ADR-0007. — Harshal

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

- **2026-07-27** — [Phase 3] **3a complete — DoD met.** Inverted index, BM25 ranking, boolean
  queries, and the shared query parser are built, tested, and wired to the `SearchService` RPC.
  - **`src/search/inverted_index.*`** — `term -> [(doc_id, tf, positions)]`, kept sorted by
    doc_id (required by the merges, by ADR-0007's delta encoding, and by skip pointers).
    Transparent hashing so query lookups don't allocate.
  - **`src/search/postings.*`** — `Intersect`/`Union`/`Difference` as linear merges, plus
    hand-written **skip pointers** (√n stride) to leap runs of non-matching ids.
  - **`src/search/ranker.*`** — BM25 exactly per `concepts/bm25.md` (smoothed non-negative IDF,
    k1=1.5/b=0.75, bounded size-K min-heap, deterministic tie-break on doc_id).
  - **`src/common/query/parser.*`** (new `atlas_query` lib) — recursive descent, `NOT`>`AND`>`OR`,
    parentheses. **Deliberately outside the search shard** so Phase 4's coordinator parses once
    and ships the tree (resolves the `search.proto` co-design TODO). Operators are UPPERCASE
    keywords matched *before* analysis — `not` is itself a stop word. Term analysis is injected,
    keeping the module dependency-free.
  - **`src/search/search_engine.*`** — parse → boolean select → BM25 rank. Implicit operator is
    **OR** (BM25 ranks; recall beats returning nothing).
  - **`src/search/search_service.*`** (new `atlas_search_service` lib) — `IndexDocument`/`Search`/
    `Stats` over gRPC, registered in `atlas_node` so every node serves its own shard (ADR-0006).
    Kept a separate target so `atlas_search` stays proto-free. `Suggest` remains UNIMPLEMENTED (3b).
  - **DoD verified** via `atlas_search_demo`: 17 docs indexed in ~32 ms; queries in
    **0.03–0.09 ms**; `consistent-hashing.md` tops "consistent hashing ring" and `bm25.md` tops
    "bm25 ranking". **70 tests** green, also green under `-DATLAS_SANITIZE=undefined`.
  - Concept notes **inverted-index** and **boolean-phrase-search** written.
  - **Known gaps (3b):** no phrase matching (positions are stored and ready), no posting-list
    compression (still in-memory, not RocksDB), no incremental update/delete — re-indexing a
    `file_id` appends a duplicate — no autocomplete/spell-correction, and `ScoredDoc.snippet` is
    empty (needs byte offsets from the tokenizer). — Harshal

- **2026-07-25** — [Phase 3] Search track opened (`phase-3/search-design`). **ADR-0007** drafted
  (*Proposed*): inverted-index format = delta+varint gap encoding in two RocksDB column families,
  postings carry `(docId_gap, tf, positions)` — positions stored up front so 3b phrase search
  needs no re-index. Build slice 1 landed: **`src/search/` text pipeline** — tokenizer,
  ~120-word stop list, **hand-written Porter stemmer**, composed by `Analyze()`; `atlas_search`
  static lib (no proto/gRPC dependency, so it builds against a local corpus independent of the
  storage track) + `atlas_search_tests`. Concept note **tokenization-stemming** written.
  **Verified through the real toolchain:** `cmake --preset default && cmake --build build &&
  ctest` is green on macOS/arm64 — 18 gtest cases in `atlas_search_tests`, plus the existing
  `atlas_tests`. Also green under `-DATLAS_SANITIZE=undefined`. — Harshal

- **2026-07-25** — [Phase 0] **Local toolchain bootstrapped + three real bugs fixed** (macOS 26.5.2
  / arm64 / Apple clang 17 / CMake 4.1 / vcpkg baseline `3ddaad9be`, deps build ~35 min cold):
  - **`Dockerfile` used `git clone --depth 1` for vcpkg** — the same trap that broke the first CI
    run (a shallow clone of vcpkg's tip lacks the pinned `builtin-baseline` commit, so Configure
    can't resolve deps). `docker compose up --build` was therefore **broken**, i.e. Phase 0's DoD
    was not actually met. Fixed: full clone + `checkout $(jq -r .builtin-baseline vcpkg.json)`,
    `jq` added to the image, `COPY vcpkg.json` hoisted above the clone.
  - **`README.md` told humans to do the same `--depth 1`** — fixed, plus `ctest` step, macOS
    prereqs, and honest first-build timing.
  - **ASan is unusable on this machine**: `-fsanitize=address` hangs in dyld's initializers
    before `main()`, reproducible with an empty `main()`. `ATLAS_SANITIZE` is now a *string*
    (`ON` = address,undefined for Linux CI/Docker · `undefined` = UBSan-only for macOS · `OFF`),
    so macOS devs get a working sanitizer. UBSan verified green.
  - Non-issues ruled out: CMake 4.1 doesn't threaten the ports (vcpkg fetches its own CMake
    4.3.3 for them). Docker Desktop *is* installed but was never launched, so it never created
    `~/.docker` or its CLI symlinks — **launch it once** before `docker compose up`.
  - Installed: `pkg-config`, `autoconf`, `automake`, `clang-format` (all were missing);
    `VCPKG_ROOT` + Docker's bin dir exported from `~/.zshrc`.
  - Still open: `clang-tidy` isn't installed locally (needs `brew install llvm`), so CI's
    advisory tidy step is the only thing running it. `vcpkg.json` deliberately still lists only
    `grpc`+`gtest` — `rocksdb`/`boost` are not needed until Phase 1, and adding them now would
    cost build time and invalidate CI's cache key. — Harshal


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
- [x] **ADR-0007** — inverted-index format & posting-list compression — *Accepted*.
- [x] **Phase 3a** — text pipeline (tokenize → stop words → Porter stem) + `tokenization-stemming` note.
- [x] **Phase 3a** — inverted index (`term → posting list`), in-memory + `inverted-index` note.
- [x] **Phase 3a** — BM25 ranker over the index (per `concepts/bm25.md`).
- [x] **Phase 3a** — boolean AND/OR/NOT + skip pointers + `boolean-phrase-search` note.
- [x] **Phase 3a** — seed corpus loader + `atlas_search_demo` DoD check (sub-second ranked query).
- [x] **Phase 3** — shared query-parser module (`atlas_query`; shard now, coordinator in Phase 4).
- [x] **Phase 3b** — phrase search (positional), field filters, incremental indexing + Compact().
- [x] **Phase 3b** — posting-list compression (delta+varint) + index Serialize/Load → ADR-0007.
- [x] **Phase 3b** — Trie autocomplete (`Suggest` RPC) + BK-tree/Levenshtein spell correction.
- [ ] **Phase 3b** — RocksDB persistence for the index (the other half of ADR-0007).
- [ ] **Phase 3b** — snippets in `ScoredDoc` (needs byte offsets from the tokenizer).
- [ ] **Phase 3b** — automatic compaction trigger; decrement vocabulary on delete.
- [ ] **Runnable demo** (PR #5 review): `docker compose up` gives storage+search nodes but no
  metadata service, and there's no client binary — the M1 demo runs only in-process inside
  `phase1_e2e_test`. Needs an `ATLAS_ROLE=metadata|storage` switch + a small `atlas put/get` CLI +
  a compose file with a metadata service. **Before the demo.**
- [ ] **Phase 1 leftover** — node-join chunk migration (ring property proven; physical move deferred
  to Phase 2, which shares the re-replication machinery).

## Resolved decisions

- Deps: **vcpkg** (0003) · Metadata: **single node for M1** (0005) · Search sharding:
  **partition by document** (0006) · Engine: **RocksDB** (0002) · Language: **C++20** (0001).

## Open questions to resolve together

- ~~Boolean/phrase/filter parsing — in the search shard or the coordinator?~~ **Resolved** (Phase 3
  kickoff): the **coordinator** parses once long-term; until it exists, the parser lives in a
  **shared, standalone unit-tested module** the shard calls now and the coordinator calls in
  Phase 4. Candidate ADR-0008 if we want it recorded formally.
- Expose per-node free/used bytes in heartbeats for placement/balancing? (`storage.proto` TODO)
- Replication ack semantics for Phase 1 (primary + how many replicas before we ack)? → ADR-0004

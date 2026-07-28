# Atlas — Progress Log

Running changelog so either of us can resume the other's thread cold. Newest first.
One entry per meaningful landing (merged PR, phase milestone, decision). Keep it terse.

Format: `YYYY-MM-DD — [phase] what changed — who`

---

## 2026-07

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
- [ ] **ADR-0004** — replication & consistency (ack semantics) — Phase 1.
- [x] **ADR-0007** — inverted-index format & posting-list compression — *Proposed, needs joint review*.
- [x] **Phase 3a** — text pipeline (tokenize → stop words → Porter stem) + `tokenization-stemming` note.
- [x] **Phase 3a** — inverted index (`term → posting list`), in-memory + `inverted-index` note.
- [x] **Phase 3a** — BM25 ranker over the index (per `concepts/bm25.md`).
- [x] **Phase 3a** — boolean AND/OR/NOT + skip pointers + `boolean-phrase-search` note.
- [x] **Phase 3a** — seed corpus loader + `atlas_search_demo` DoD check (sub-second ranked query).
- [x] **Phase 3** — shared query-parser module (`atlas_query`; shard now, coordinator in Phase 4).
- [ ] **Phase 3b** — phrase search (positions already stored), filters, incremental indexing.
- [ ] **Phase 3b** — posting-list compression (delta+varint) + RocksDB persistence → ADR-0007.
- [ ] **Phase 3b** — Trie autocomplete (`Suggest` RPC) + BK-tree/Levenshtein spell correction.
- [ ] **Phase 3b** — snippets in `ScoredDoc` (needs byte offsets from the tokenizer).

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

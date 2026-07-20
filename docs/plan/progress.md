# Atlas — Progress Log

Running changelog so either of us can resume the other's thread cold. Newest first.
One entry per meaningful landing (merged PR, phase milestone, decision). Keep it terse.

Format: `YYYY-MM-DD — [phase] what changed — who`

---

## 2026-07

- **2026-07-20** — [Phase 0] Repo initialized (`main`), docs scaffolded: overview, roadmap,
  collaboration model, system architecture, ADR-0001 (C++20), concepts hub + template +
  two flagship notes (consistent-hashing, BM25). Decisions locked: **C++20 core**,
  **Milestone 1 = distributed core demo by Jul 30**, **hybrid co-design + PR workflow**. — Ojas

---

## Backlog / next up

- [ ] **Phase 0** — CMake + dependency manager (vcpkg/Conan) decision → ADR-0002.
- [ ] **Phase 0** — Draft `proto/` contracts (Metadata, Storage, Search, Coordinator) — *co-design together*.
- [ ] **Phase 0** — Single-node gRPC health-check skeleton + docker-compose 3-node cluster.
- [ ] **Phase 0** — GitHub Actions CI (build + test).
- [ ] **ADR-0002** — storage engine (RocksDB vs LevelDB vs SQLite).
- [ ] **ADR-0003** — replication & consistency model.

## Open questions to resolve together

- vcpkg vs Conan for dependencies?
- Single metadata node for M1 (simpler) vs Raft-replicated metadata from the start?
- Sharding key for the search index — by document, by term, or by node-owned corpus?

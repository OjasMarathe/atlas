# ADR-0002 — Embedded storage engine: RocksDB

**Status:** Accepted (2026-07-21)

## Context

Every stateful node needs a local, embedded, durable key-value store: storage nodes keep
chunk bytes, the metadata service keeps the file/chunk map + ring, and search shards persist
the inverted index. Per [ADR-0001](0001-language-cpp20.md)'s mitigations, we **buy the local
storage engine and spend our effort on the distributed layer above it** — we don't hand-roll
an on-disk store.

## Options considered

- **RocksDB** — LSM-tree KV store (Facebook). Built-in WAL, block checksums, compaction,
  column families, tunable. Battle-tested. Verbose C++ API.
- **LevelDB** — RocksDB's simpler ancestor. Fewer features (no column families, weaker tuning),
  less active.
- **SQLite** — relational, B-tree, transactional. Excellent for the *metadata* service's
  structured queries; not ideal as a high-write blob store for chunks.
- **LMDB** — mmap'd B+tree, superb read latency; smaller ecosystem, write-amplification story
  differs.

## Decision

**RocksDB is the default embedded engine** for chunk storage, metadata, and index persistence.
Use **column families** to separate namespaces within a node. We may additionally use
**SQLite for the metadata service** if relational queries prove convenient (revisit in Phase 1).

## Consequences

- **Positive:** production-grade durability (WAL), integrity (block checksums), and compaction
  for free; column families keep concerns separated; one dependency covers three services.
- **Negative:** heavyweight dependency and a verbose C++ API; a learning curve on tuning.
- **Learning note:** we still *implement and document* WAL and checksums as concepts (that's the
  point). Where we instead rely on RocksDB's built-ins, the concept note says so explicitly, so
  we understand both the idea and what the library does for us.

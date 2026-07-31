# Architecture Decision Records (ADRs)

An **ADR** captures one significant architectural decision: the context, the options we
weighed, what we chose, and the consequences we accept. They're short, immutable once
accepted (we supersede rather than edit), and numbered in order.

Why we keep them: they force us to think before we build, they let the other person
understand *why* something is the way it is without re-litigating it, and — bluntly — a repo
with thoughtful ADRs signals engineering maturity to anyone reviewing it.

## Format

Each ADR: **Status** (Proposed / Accepted / Superseded) · **Context** · **Options
considered** · **Decision** · **Consequences** (positive & negative) · optional **Mitigations**.
Copy an existing one as a template.

## Index

| # | Title | Status |
|---|---|---|
| [0001](0001-language-cpp20.md) | Core language: C++20 | Accepted |
| [0002](0002-storage-engine-rocksdb.md) | Embedded storage engine: RocksDB | Accepted |
| [0003](0003-dependency-manager-vcpkg.md) | C++ dependency manager: vcpkg | Accepted |
| [0005](0005-metadata-single-node-m1.md) | Metadata: single node for M1 | Accepted (M1) |
| [0004](0004-replication-consistency.md) | Replication & consistency (3×, W=2/R=1) | Accepted |
| [0006](0006-search-index-document-partitioned.md) | Search index: partition by document | Accepted |
| [0009](0009-failure-detection-and-healing.md) | Failure detection & self-healing model | Accepted |

### Upcoming ADRs to write

- **0007** — Inverted-index on-disk format & posting-list compression — *Phase 3* (drafted on the
  search branch).
- **0008** — Query-parsing location: coordinator vs. search shard — *Phase 3* (reserved for the
  search track; Phase 2 skipped to 0009 to avoid a collision).

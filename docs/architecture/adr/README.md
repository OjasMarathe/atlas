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

### Upcoming ADRs to write

- **0002** — Storage engine: RocksDB vs LevelDB vs SQLite.
- **0003** — Dependency manager & build: vcpkg vs Conan.
- **0004** — Replication & consistency model (quorum, ack semantics).
- **0005** — Metadata: single node for M1 vs Raft-replicated from the start.
- **0006** — Search index sharding key (by document vs by term).
- **0007** — Inverted-index on-disk format & posting-list compression.

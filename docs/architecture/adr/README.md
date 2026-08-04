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
| [0004](0004-replication-consistency.md) | Replication & consistency (3×, W=2/R=1) | Accepted |
| [0005](0005-metadata-single-node-m1.md) | Metadata: single node for M1 | Accepted (M1) |
| [0006](0006-search-index-document-partitioned.md) | Search index: partition by document | Accepted |
| [0007](0007-inverted-index-format-compression.md) | Inverted-index format & posting-list compression | Accepted |
| [0009](0009-failure-detection-and-healing.md) | Failure detection & self-healing model | Accepted |
| [0010](0010-global-scoring-dfs.md) | Global BM25 statistics for distributed ranking (DFS) | Accepted |
| [0011](0011-async-model-grpc-completion-queue.md) | Async model: gRPC CompletionQueue, not Boost.Asio | Accepted |

### Upcoming ADRs to write

- **0008** — Query-parsing location: coordinator vs. search shard — *Phase 3/4*. **Effectively
  resolved in Phase 4** and recorded inline in `proto/search.proto`: the shard parses, because
  parsing costs microseconds against milliseconds of network and a query string on the wire is
  easier to debug than a serialized AST. The parser still lives in the shared `src/common/query`
  module so the coordinator *can* take over when there is a reason to. Worth promoting to a real
  ADR if we ever move it.

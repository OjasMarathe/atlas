# ADR-0011 — Async model: gRPC CompletionQueue, not Boost.Asio

**Status:** Accepted (2026-08-03)

## Context

The Phase 4 roadmap entry reads: *"Connection pool + thread pool + async networking (Boost.Asio)
for concurrency."* Boost.Asio was named during Phase 0 planning, before the shape of the system
was settled, and it is still listed in the README's tech-stack table.

By the time Phase 4 was implemented the concurrency requirement had become concrete and narrow:
the coordinator must issue an RPC to every search shard, hold them all in flight simultaneously,
and collect the answers as they arrive — so that a fan-out costs the slowest shard rather than the
sum, without dedicating a thread to each shard.

That requirement is worth stating precisely, because it is *entirely* about gRPC calls. Atlas has
no raw sockets. Every network operation in the system — storage, metadata, search, coordinator —
is a gRPC RPC.

## Options considered

**1. Boost.Asio, as the roadmap says.**
A mature, well-designed async I/O library with its own `io_context` event loop, and the reference
implementation of the executor model heading into the C++ standard. Adds `boost-asio` to
`vcpkg.json`.

**2. gRPC's `CompletionQueue`.**
Already present — gRPC ships its own event loop and exposes an async API over it.
`PrepareAsyncX` / `StartCall` / `Finish(&response, &status, tag)`, then drain with `cq.Next(&tag,
&ok)`.

**3. Blocking calls on the thread pool.**
Fan out by submitting one blocking RPC per shard to `ThreadPool` and joining the futures.

**4. gRPC's callback API.**
The newer, higher-level async interface: completions are delivered to a callback rather than
drained from a queue.

## Decision

**Use gRPC's `CompletionQueue` (option 2). Do not add Boost.Asio.**

The decisive argument is that adding Asio would put **two event loops in one process**. gRPC's
loop would still own and drive every socket Atlas has, because every operation is an RPC; Asio's
`io_context` would sit alongside it managing nothing, with its own threads and its own readiness
state, unaware of the other's queue depth. That is not extra capability — it is a second scheduler
competing for the same cores to supervise sockets it does not own.

Secondary, but real: `boost-asio` in `vcpkg.json` changes the manifest hash, which invalidates the
CI vcpkg binary cache and forces a cold dependency build on the next run. Worth paying for
something needed; not worth paying for something unused.

Option 3 was rejected because it ties in-flight RPCs to threads — N shards × M concurrent queries
threads, almost all parked in `recv()`. That is the specific scaling limit async I/O exists to
remove.

Option 4 is genuinely better ergonomics and the direction gRPC is moving. Not chosen now only
because the completion-queue form makes the mechanism (tags, lifetimes, deadlines) explicit, which
is the point of a project whose goal is understanding the internals. Migration is mostly
mechanical and is recorded as the natural follow-up.

## Consequences

**Positive**

- One event loop, one set of threads, one place where I/O concurrency is reasoned about.
- No new dependency; CI cache stays valid.
- The fan-out costs one thread regardless of shard count. `coordinator_test` sustains 32
  concurrent clients at ~4900 queries/second against four in-process shards.
- Deadlines are per-call and enforced by the same loop that carries the request, so "wait for N
  completions" is guaranteed to terminate.

**Negative**

- **The roadmap and README name a library we do not use.** Both are updated to point here, but
  anyone reading the original plan will notice the divergence — hence this ADR.
- **The completion-queue API is low-level and easy to misuse.** Response, context and status must
  outlive the RPC; a local would be written into after destruction. Contained by giving every
  in-flight call a heap-allocated `AsyncCall` struct that owns all of it.
- **No async abstraction for future non-gRPC I/O.** When Phase 6's crawler needs to fetch HTTP
  from arbitrary hosts, gRPC's loop is the wrong tool and this decision does not cover it.

## When to revisit

Adopt Asio (or `io_uring`, or C++23 `std::execution`) when Atlas acquires I/O that gRPC does not
own — the Phase 6 crawler's HTTP fetches being the concrete, expected case. At that point Asio
would be managing its own sockets rather than shadowing gRPC's, and the objection above
disappears.

Independently, migrating the existing fan-out to gRPC's callback API is worthwhile whenever the
completion-queue bookkeeping starts costing more than it teaches.

## References

- `docs/concepts/async-io.md` — the mechanism, the lifetime rule, and the thread-pool split.
- [gRPC C++ async](https://grpc.io/docs/languages/cpp/async/) ·
  [callback API](https://grpc.io/docs/languages/cpp/callback/).
- ADR-0001 (C++20 core), which sets the "understand the internals" bar this decision is weighed
  against.

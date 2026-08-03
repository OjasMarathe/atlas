# Connection Pool

**Area:** Concurrency · **Phase:** 4 · **Status:** written

## TL;DR

Reuse a bounded set of connections to each endpoint instead of opening one per request. The
classic motivation — handshake cost — applies less to gRPC than to a database, because a gRPC
channel is already thread-safe and multiplexes concurrent calls over one HTTP/2 connection. The
pool's real jobs here are amortizing channel setup, raising the concurrent-stream ceiling, and
making "how many in-flight calls to one shard" a number we set rather than one we discover in
production.

## The problem it solves

Each query fans out to every shard, twice. Creating a channel per call means, per query per shard:
a DNS resolution, a TCP handshake (1 RTT), and for TLS another 1-2 RTTs. On a LAN that is
~0.1-1 ms of pure setup added to a request whose useful work is a few milliseconds; over the
internet it dwarfs the work entirely.

But the honest version of this note has to state the caveat up front, because it changes what the
class is *for*:

> **A gRPC channel is already thread-safe and multiplexes.** You can share one channel across
> every thread in the process and it works correctly. Unlike a PostgreSQL connection — which is a
> single-threaded session holding transaction state — a gRPC channel is not a resource that one
> caller must exclusively own.

So this pool is not required for correctness. What it buys:

1. **Amortized setup.** Channels are created once per endpoint and reused, not rebuilt per call.
2. **A higher concurrency ceiling.** HTTP/2 caps concurrent streams per connection
   (`SETTINGS_MAX_CONCURRENT_STREAMS`, commonly 100). Past that, requests queue *inside* the
   channel and present as server slowness with no server-side evidence. A handful of channels per
   endpoint raises the ceiling.
3. **A bound.** Unbounded channel creation doesn't remove the resource limit, it just moves it
   somewhere harder to see — file descriptors, or the peer's connection table.

## How it works

Per endpoint: a set of idle connections, a count of live ones (idle + leased), and a
`condition_variable`.

```
Acquire(addr):
    lock
    while idle.empty() and live >= max_per_endpoint:
        wait(available)              ← all connections are out; block
    if idle not empty:
        return lease(idle.pop())     ← reuse
    live += 1
    unlock                           ← channel creation can resolve DNS; don't hold the lock
    return lease(new channel)

Release(addr, conn):                 ← runs from the lease's destructor
    lock; idle.push(conn); notify_one(available)
```

### Leases are RAII, and that's the point

```cpp
{
  auto lease = pool.Acquire(shard.address);
  lease->Search(&context, request, &response);
}   // returned here — including if Search threw, or we returned early
```

A `get()`/`put()` pair leaks a connection on every early return and every exception. Since a leak
here means the pool permanently believes a connection is in use, N leaks deadlock the endpoint
forever. Tying the return to a destructor makes that class of bug unwritable.

The lease is move-only: copying it would return the same connection twice.

### One subtlety in the container

`Endpoint` holds a `condition_variable`, which is neither copyable nor movable, and callers wait
on a reference to it across the map's rehashes. This is safe only because `std::unordered_map`
guarantees **reference stability** — rehashing relinks buckets but never moves elements. The same
code over a `std::vector` would be a use-after-free the first time it grew.

## Our implementation in Atlas

- **Where it lives:** [`src/common/pool/connection_pool.h`](../../src/common/pool/connection_pool.h).
  Templated on the gRPC service, so `ConnectionPool<atlas::SearchService>` pools `SearchService::Stub`s.
- Owned by `QueryCoordinator`; `CoordinatorOptions::connections_per_shard` (default 4) is the cap.
- Async calls hold their lease inside the `AsyncCall` struct for the RPC's whole lifetime, so a
  connection isn't returned while a completion queue still expects to write into it.
- Stats (`created`, `reused`, `waited`, `live`, `endpoints`) exist to make the behaviour testable:
  `pool_test` asserts that 10 sequential acquisitions of one endpoint produce **1 created, 9
  reused**, and that a third acquisition against a cap of 2 genuinely blocks until a lease is
  released.

## Complexity & trade-offs

| | |
|---|---|
| Acquire (idle available) | O(1) |
| Acquire (at cap) | blocks until a release |
| Release | O(1) |
| Memory | `endpoints × max_per_endpoint` channels |

**Sizing is the trade-off.** Too small and callers block on `Acquire` — a self-inflicted queue.
Too large and you hold file descriptors and peer connection slots for nothing. For gRPC
specifically the useful rule is that one channel handles ~100 concurrent streams, so
`max_per_endpoint = 4` supports ~400 concurrent calls per shard, comfortably beyond anything M1
generates.

## Failure modes & edge cases

- **Cap exhaustion** blocks indefinitely — there is no acquire timeout. With RAII leases and
  deadline-bounded RPCs every lease is guaranteed to return, so this cannot deadlock in current
  usage; it *would* be a real risk the moment a caller could hold a lease across an unbounded
  operation. A `try_acquire_for` is the natural hardening.
- **Broken connections are not detected.** A pooled channel to a shard that died is handed out and
  the RPC fails. gRPC channels reconnect automatically on the next call, so this self-heals, but
  the failing call is spent discovering it. A database pool must validate on borrow; gRPC's
  built-in reconnection is why this one doesn't.
- **A dead endpoint's connections are never evicted.** Membership churn slowly accumulates idle
  channels for nodes that have left. Bounded by cap × endpoints-ever-seen, and untouched for M1 —
  the same GC gap Phase 2 left for departed nodes.
- **No idle timeout**, so connections persist for process lifetime.

## Alternatives we considered

- **One shared channel per endpoint, no pool.** Genuinely viable for gRPC and simpler — this is
  what `AtlasClient` does with its `unordered_map<address, Stub>`. Rejected for the coordinator
  because the fan-out is the hot path where the stream ceiling and an explicit bound matter, and
  because "implement a connection pool" is a Phase 4 deliverable and a concept worth owning.
- **A channel per call.** Correct, and pays full setup every time.
- **gRPC's own channel pooling** (`GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL`) — real, and lower-level:
  it pools subchannels beneath the channel abstraction rather than giving callers lease
  semantics or a visible bound.
- **Validate-on-borrow** (`Channel::GetState`) — the database-pool reflex. Skipped because gRPC
  reconnects transparently, so the check would cost a syscall to learn something the next RPC
  discovers anyway.

## Interview Q&A

**Q: Why does a gRPC connection pool matter less than a database connection pool?**
Because a gRPC channel is thread-safe and multiplexes many concurrent RPCs over one HTTP/2
connection, while a database connection is a single-threaded session holding transaction state.
For gRPC the pool is about amortizing setup, raising the concurrent-stream ceiling, and bounding
resources — not about mutual exclusion.

**Q: Why must the lease be RAII?**
Any manual release is skipped on an early return or an exception. A leaked lease permanently
consumes a slot, so enough leaks deadlock the endpoint. A destructor runs on every path.

**Q: What happens when the pool is exhausted?**
`Acquire` blocks on a condition variable until a lease is destroyed. That is deliberate
backpressure. It is safe here only because every RPC has a deadline, so every lease is guaranteed
to come back.

**Q: How do you size it?**
From the concurrency you intend to support, not from guesswork: for gRPC, ~100 concurrent streams
per channel, so `ceil(peak_concurrent_calls_per_endpoint / 100)` plus headroom.

**Q: How do you avoid handing out a dead connection?**
For gRPC you largely don't need to — channels reconnect automatically, so a failed call is the
detection mechanism and the next one succeeds. A pool over a protocol without that (a raw socket,
a database session) must validate on borrow or track liveness explicitly.

## References

- [gRPC Core concepts — Channels](https://grpc.io/docs/what-is-grpc/core-concepts/#channels) and
  [performance best practices](https://grpc.io/docs/guides/performance/) (the ~100-stream guidance).
- RFC 9113 §5.1.2 — HTTP/2 `SETTINGS_MAX_CONCURRENT_STREAMS`.
- [thread-pool](thread-pool.md) · [async-io](async-io.md) · [scatter-gather](scatter-gather.md)

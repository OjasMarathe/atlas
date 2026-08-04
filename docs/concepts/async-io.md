# Asynchronous I/O

**Area:** Concurrency · **Phase:** 4 · **Status:** written

## TL;DR

Blocking I/O ties one request to one thread, so concurrency costs stacks and context switches.
Asynchronous I/O lets **one** thread hold many operations in flight and react as each completes.
Atlas's query fan-out uses gRPC's `CompletionQueue` for exactly this: one thread issues an RPC to
every shard and gathers the answers as they land, so a fan-out costs the *slowest* shard rather
than the sum — without a thread per shard.

## The problem it solves

The coordinator must query N shards. With blocking calls:

```cpp
for (shard : shards) hits += shard.Search(q);   // 4 shards × 5 ms = 20 ms
```

Latency is the **sum**, and it grows as you add shards — precisely backwards, since more shards is
how the system scales.

The obvious fix is a thread per shard, which makes latency the max. But it buys that with a thread
per shard *per concurrent query*: 100 shards × 100 queries = 10,000 threads, nearly all of them
parked inside `recv()`. Each costs a stack and a scheduler slot to do nothing. The work is
**I/O-bound** — the thread isn't computing, it's waiting — and dedicating an OS thread to waiting
is the mismatch.

Async I/O separates *starting* an operation from *learning it finished*. One thread starts N
operations and then asks the kernel "tell me which of these is ready."

## How it works

Underneath every async runtime is a readiness or completion primitive: `epoll` (Linux), `kqueue`
(BSD/macOS), IOCP (Windows), `io_uring` (modern Linux). Two models:

- **Readiness** (`epoll`): "this socket is now readable" — you then perform the read.
- **Completion** (IOCP, `io_uring`): "your read finished, here are the bytes."

gRPC exposes a completion model through `CompletionQueue`:

```
     ┌─ PrepareAsyncSearch(shard1) ─ StartCall ─ Finish(&resp1, &status1, tag1) ─┐
one ─┼─ PrepareAsyncSearch(shard2) ─ StartCall ─ Finish(&resp2, &status2, tag2) ─┤─▶ CompletionQueue
thread┼─ PrepareAsyncSearch(shard3) ─ StartCall ─ Finish(&resp3, &status3, tag3) ─┤
     └─ PrepareAsyncSearch(shard4) ─ StartCall ─ Finish(&resp4, &status4, tag4) ─┘
                                                          │
              while (cq.Next(&tag, &ok)) { ... }  ◀───────┘   completions, in arrival order
```

The **tag** is an opaque `void*` you supply and get back on completion — the correlation between
"an answer arrived" and "which request it answers." Here the tag is the address of the call's own
state object.

### The lifetime rule, which is where bugs live

Everything the completion queue will write into must outlive the call: the `ClientContext`, the
response message, the status. They cannot be locals in the issuing loop — that loop returns long
before the RPC completes, and the queue would write into destroyed stack memory. So each in-flight
call owns a heap-allocated struct:

```cpp
template <typename Response>
struct AsyncCall {
  grpc::ClientContext context;                        // must outlive the RPC
  Response response;                                  // the queue writes here
  grpc::Status status;                                // and here
  ConnectionPool<SearchService>::Lease lease;         // held until completion
  std::unique_ptr<grpc::ClientAsyncResponseReader<Response>> reader;
};
```

### Why the gather loop can't hang

```cpp
for (std::size_t i = 0; i < calls.size(); ++i) {
  queue.Next(&tag, &ok);      // blocks until *something* completes
  ...
}
```

Waiting for exactly `calls.size()` completions is only safe because **every context carries a
deadline**. A deadline is itself a completion — an unreachable shard produces
`DEADLINE_EXCEEDED`, not silence. Without deadlines this loop would block forever on the first
dead shard. That is the invariant the whole structure rests on.

Afterwards the queue is shut down and drained, so no completion outlives the objects it refers to.

## Our implementation in Atlas

- **Where it lives:** [`src/coordinator/coordinator.cpp`](../../src/coordinator/coordinator.cpp)
  — `CollectGlobalStats` and `FanOut`, both built on the `AsyncCall` pattern above.
- Both rounds of a DFS query fan out asynchronously, so a query costs ~2 × the slowest shard
  rather than 2 × the sum.
- Slow shards are dropped, not retried — `shards_responded` reports the shortfall. See
  [scatter-gather](scatter-gather.md).

### Why not Boost.Asio

The roadmap named Boost.Asio for this phase. It isn't used, and the reasoning is recorded in
[ADR-0011](../architecture/adr/0011-async-model-grpc-completion-queue.md):

Atlas speaks gRPC everywhere, and gRPC ships its own event loop. Adding Asio would put a second
event loop in the same process, each with its own threads, neither aware of the other's readiness
state — every RPC would still go through gRPC's loop regardless, so Asio's would be managing
sockets it doesn't own. The concurrency Phase 4 actually needed was "issue N RPCs and collect
them," which is precisely what `CompletionQueue` is. Asio earns its place when you need async over
sockets gRPC does *not* own — a raw TCP protocol, or the Phase 6 crawler's HTTP fetches. That is
where to revisit it.

### Async vs. the thread pool

Atlas has both, and the split is deliberate:

| | Use |
|---|---|
| [Thread pool](thread-pool.md) | **CPU-bound** work we own — parallel compute, load-test clients |
| Completion queue | **I/O-bound** waiting — the shard fan-out |

Blocking the pool on network waits would occupy threads that do nothing but sleep. Running
compute on the completion-queue thread would stall every other in-flight RPC. Knowing which of the
two a workload is, is the entire skill.

## Complexity & trade-offs

| | Thread-per-shard | Completion queue |
|---|---|---|
| Threads for N in-flight RPCs | N | 1 |
| Memory | N stacks (~8 MB virtual each) | N small structs |
| Latency | max(shards) | max(shards) |
| Code complexity | low | meaningfully higher |

**Async is harder to write and harder to debug.** Control flow is inverted — there is no stack
trace spanning "issued" and "completed," and lifetime errors surface as memory corruption rather
than compile errors. That cost is real, and at 4 shards a thread-per-shard version would perform
identically. It is paid here because the fan-out is the one path whose concurrency grows with both
shard count and query rate, and because C++20 has no coroutine-based gRPC API to make it pleasant.

## Failure modes & edge cases

- **Missing deadlines** turns the gather loop into an indefinite block. Every context sets one.
- **Stack-allocated response/context** is the canonical async bug: the queue writes into freed
  memory after the issuing function returns. Heap-owned `AsyncCall` prevents it.
- **`ok == false` from `Next`** means the operation was cancelled or the queue is shutting down —
  distinct from "completed with a non-OK status." Both are handled as "this shard didn't answer."
- **Not draining before destruction** leaves completions referring to freed tags. The queue is
  shut down and drained at the end of every fan-out.
- **A completion queue is not thread-safe for concurrent `Next` on one tag** — Atlas uses one
  queue per fan-out, owned by the calling thread, which sidesteps sharing entirely.

## Alternatives we considered

- **Blocking calls on the thread pool.** Simplest, and adequate at today's scale. Rejected because
  it ties in-flight RPCs to threads, which is the thing that stops scaling first.
- **Boost.Asio.** A second event loop next to gRPC's — see ADR-0011.
- **gRPC callback API** (`grpc::ClientContext` + `async_unary`). Genuinely nicer ergonomics and
  the modern recommendation; it moves completion handling into a callback rather than a queue
  drain. Worth migrating to, and mostly a mechanical change from here.
- **C++20 coroutines** over the callback API. The cleanest expression of this code
  (`co_await` each shard, gather). No standard executor in C++20 makes it a build-your-own affair;
  revisit with C++23's `std::execution`.

## Interview Q&A

**Q: Why is async I/O better than a thread per connection?**
Because the threads aren't computing, they're waiting. A blocked thread still costs a stack and a
scheduler slot, so concurrency is capped by memory rather than by work. One thread on a completion
queue holds thousands of operations in flight for the cost of one stack.

**Q: What's the difference between readiness and completion models?**
Readiness (`epoll`, `kqueue`) tells you a socket *can* be operated on and you then perform the
operation. Completion (IOCP, `io_uring`, gRPC's `CompletionQueue`) tells you the operation is
*already done* and hands you the result. Completion models avoid the extra syscall and are easier
to use for buffered I/O.

**Q: What is the tag in a gRPC completion queue?**
An opaque `void*` you pass when starting an operation and get back when it completes — the
correlation between a completion and the request that produced it. Typically the address of the
heap-allocated state for that call.

**Q: What's the most common bug in this style of code?**
Lifetime. The context, response and status must outlive the RPC, so they cannot be locals in the
function that issued it. The second most common is omitting deadlines, which turns "wait for N
completions" into a permanent hang.

**Q: You said async, but you also have a thread pool. Why both?**
They solve different problems. The completion queue handles waiting on I/O without burning
threads; the pool runs CPU work in parallel. Using either for the other's job is a performance
bug — blocked pool threads, or a stalled event loop.

## References

- [gRPC C++ async API](https://grpc.io/docs/languages/cpp/async/) and the
  [callback API](https://grpc.io/docs/languages/cpp/callback/).
- Dan Kegel, *The C10K problem* — the original statement of why thread-per-connection stops.
- Jens Axboe, *Efficient IO with io_uring* (2019).
- [thread-pool](thread-pool.md) · [connection-pool](connection-pool.md) ·
  [scatter-gather](scatter-gather.md) ·
  [ADR-0011](../architecture/adr/0011-async-model-grpc-completion-queue.md)

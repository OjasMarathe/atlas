# Thread Pool

**Area:** Concurrency · **Phase:** 4 · **Status:** written

## TL;DR

A fixed set of worker threads pulling from a shared task queue. It exists for two reasons that
get conflated: it **amortizes** thread creation, and — the one that actually matters in a server —
it **bounds** concurrency, so load turns into a queue instead of into thousands of threads.

## The problem it solves

Thread-per-task is the obvious design and it fails in two directions.

**Creation cost.** A `std::thread` costs tens of microseconds and a stack allocation (8 MB of
virtual address space by default on Linux). For a task measured in microseconds, you spend more
creating the worker than doing the work.

**Unbounded concurrency.** This is the serious one. 10,000 concurrent requests means 10,000
threads, which means the scheduler thrashes, every context switch evicts cache lines, and memory
goes to stacks. The system doesn't slow down gracefully — it collapses. A pool of N workers with a
queue in front degrades *linearly*: latency rises, throughput holds. That is the difference
between slow and down.

The pool also gives you a place to put backpressure. A queue you can see the depth of is a signal;
threads you can't count aren't.

## How it works

```
Submit(fn) ──▶ [ task queue ]  ◀── worker 1  ─┐
                    │          ◀── worker 2   │  all blocked on the same
                    │          ◀── worker 3   │  condition_variable
                    └──────────◀── worker 4  ─┘
```

Each worker loops:

```cpp
while (true) {
  unique_lock lock(mutex);
  cv.wait(lock, [&]{ return stopping || !tasks.empty(); });
  if (tasks.empty()) return;            // stopping AND drained
  task = move(tasks.front()); tasks.pop();
  lock.unlock();
  task();                               // run OUTSIDE the lock
}
```

Three details that are easy to get wrong:

**Run the task outside the lock.** Holding the queue mutex while executing serializes the pool
into a single thread with extra steps.

**The predicate, not a bare `wait`.** `condition_variable` permits spurious wakeups, and a
notification arriving before the wait is simply lost. The predicate re-checks the condition on
every wake, which handles both.

**Drain before exiting.** Every queued task holds a `promise`. Dropping it on shutdown breaks
every caller still blocked on the matching `future` — they get `broken_promise` instead of a
result. So a stopping worker only returns once the queue is *empty*.

### Getting results (and exceptions) back

An exception escaping a thread's entry function calls `std::terminate` — the whole process dies
because one task threw. `std::packaged_task` is the fix: it captures the return value *or* the
exception into a `std::future`, and rethrows on `get()`. So:

```cpp
auto future = pool.Submit([]{ throw std::runtime_error("boom"); });
future.get();   // throws here, on the caller's thread, where it can be handled
```

## Our implementation in Atlas

- **Where it lives:** [`src/common/pool/thread_pool.h`](../../src/common/pool/thread_pool.h).
- `Submit(fn, args...)` returns `std::future<std::invoke_result_t<Fn, Args...>>` — variadic, so
  arguments are bound at submission rather than forcing the caller to write a lambda.
- `WaitIdle()` blocks until the queue is empty *and* no task is running (a second
  `condition_variable`, signalled when the last active worker finishes). "Queue empty" alone is
  the classic bug: it returns while tasks are still executing.
- `Shutdown()` is idempotent and drains; the destructor calls it, so the pool is RAII.
- Default size is `hardware_concurrency()`, falling back to 4 when that returns 0 (which it is
  permitted to do).

### Where Atlas actually uses it — and where it deliberately doesn't

This is worth being blunt about, because "add a thread pool to your server" is cargo-culted
advice. **gRPC's synchronous server already has its own thread pool.** Putting ours in front of it
would be two schedulers competing for the same cores, each unaware of the other's queue depth.

So the pool is used where Atlas owns the parallelism and gRPC does not:

- **the load test** — driving 32 concurrent clients in `coordinator_test`, which is what the DoD's
  "sustains N concurrent clients" means;
- as the general-purpose primitive for parallel work that isn't RPC fan-out.

The query fan-out itself does **not** use it. Fanning out to N shards with blocking calls would
occupy N pool threads that spend their lives in `recv()`. That work is I/O-bound, not CPU-bound,
so it belongs on a completion queue instead — see [async-io](async-io.md). Knowing which of the
two a workload is, is the whole skill.

## Complexity & trade-offs

| | |
|---|---|
| Submit | O(1) amortized, one mutex acquisition |
| Contention | Single mutex + single queue — the bottleneck at high core counts |
| Memory | N stacks (8 MB virtual, ~8-64 KB resident each) + queue |

**The single queue is the known scaling limit.** Every worker contends on one mutex, so past
roughly 8-16 cores the queue itself becomes the bottleneck. The standard fix is **work stealing**:
per-worker deques where a worker pushes/pops its own end lock-free and only steals from another
when its own is empty (Cilk, Go's scheduler, `tokio`). Deliberately not implemented — Atlas's
pool sees dozens of tasks, not millions, and a work-stealing deque is a lot of subtle code to
justify with no measurement demanding it.

## Failure modes & edge cases

- **Submit after shutdown** throws `std::runtime_error` rather than silently dropping the task —
  a dropped task means a future that never completes, i.e. a caller hung forever.
- **A task that blocks forever** permanently consumes a worker. N such tasks deadlock the pool.
  No timeout mechanism exists; tasks are trusted, which is reasonable for in-process work and
  would not be for anything user-supplied.
- **A task that submits to its own pool and waits on the result** deadlocks if every worker is
  doing the same. Classic pool footgun; avoided here by never nesting submissions.
- **`hardware_concurrency()` returning 0** is legal and handled.
- **Exceptions** are captured into the future, not propagated on the worker thread.

## Alternatives we considered

- **`std::async`** — no pool guarantee at all: implementations may launch a fresh thread per call,
  and the returned future's destructor *blocks*, which turns a fire-and-forget into a hidden join.
- **Work-stealing pool** — better at scale, much more code. Revisit with a profile that shows
  queue contention.
- **`std::jthread` per task** — cleaner lifetime handling than `std::thread`, same unbounded
  concurrency problem.
- **Letting gRPC do it** — which is exactly what we do for RPC serving. The pool is for the work
  gRPC doesn't own.

## Interview Q&A

**Q: Why a pool instead of a thread per task?**
Two reasons. Creation cost (tens of microseconds plus a stack) dominates short tasks; and more
importantly a pool *bounds* concurrency, so overload becomes a growing queue rather than thousands
of threads thrashing the scheduler. Bounded systems degrade; unbounded ones collapse.

**Q: How do exceptions get out of a worker thread?**
`std::packaged_task` stores the exception in the shared state, and `future::get()` rethrows it on
the caller's thread. Without that, an exception escaping the thread function calls
`std::terminate` and takes the process with it.

**Q: Why must shutdown drain the queue?**
Each queued task owns a promise. Destroying it un-run breaks the associated future, so anyone
waiting gets `broken_promise` instead of an answer. Workers therefore exit only once the queue is
empty.

**Q: How do you know when all work is finished?**
Not by "queue is empty" — tasks may still be running. You need a count of *active* tasks too, and
to signal when the queue is empty and that count reaches zero.

**Q: How would you size the pool?**
CPU-bound work: roughly `hardware_concurrency()`. I/O-bound work: more threads help, but that's
usually the signal you should be using async I/O rather than blocked threads. Atlas does exactly
that split — pool for compute, completion queue for the shard fan-out.

## References

- Herlihy & Shavit, *The Art of Multiprocessor Programming*, ch. 16 (work distribution / stealing).
- Blumofe & Leiserson, *Scheduling Multithreaded Computations by Work Stealing* (1999).
- Anthony Williams, *C++ Concurrency in Action*, 2nd ed., ch. 9.
- [async-io](async-io.md) · [connection-pool](connection-pool.md)

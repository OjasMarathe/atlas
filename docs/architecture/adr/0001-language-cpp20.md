# ADR-0001 — Core language: C++20

**Status:** Accepted (2026-07-20)

## Context

Atlas is a learning-first, resume-defining project. Two goals shape every technical choice:

1. **Learn distributed-systems and storage internals by implementing them ourselves.**
2. **Produce a strong software-engineering signal** — ideally a rare one.

The core services (storage nodes, metadata, search, coordinator) need a language for the
"spine." The ML/semantic module (Phase 5) will be Python and the dashboard React/TS
regardless — this decision is only about the core.

Both authors are strongest in Python/TS + ML; neither has deep prior C++ systems experience.
So this is deliberately a stretch choice, made with eyes open.

## Options considered

### Go
- **Pros:** the lingua franca of modern distributed infra (Docker, Kubernetes, etcd,
  CockroachDB). Goroutines + channels + `context` make concurrency and networking far
  easier; built-in tooling and first-class gRPC. We'd ship ~2–3× more working system in the
  same time, and spend that time on distributed-systems concepts rather than language
  friction. Directly relevant to backend/infra SWE roles.
- **Cons:** the runtime/GC hides some of the low-level machinery (manual memory, explicit
  synchronization primitives) that we specifically want to learn. Slightly less of a "hard
  systems" flex than C++.

### C++20 (chosen)
- **Pros:** maximum systems-programming depth and credibility. The concurrency primitives
  the project calls for — `std::mutex`, `std::shared_mutex`, `std::condition_variable`,
  `std::atomic`, the memory model, lock-free queues, thread pools — *are* the learning
  objective, not incidental. Strongest signal for systems-heavy roles (infra, HFT,
  databases, embedded). Matches the reference problem statement.
- **Cons:** 2–3× slower to build. Toolchain tax (CMake, dependency management, gRPC C++
  setup) is real. Memory-safety and concurrency bugs are easy to introduce and costly to
  debug. Biggest jump from our current skill set → we will finish a **smaller** slice by
  Jul 30 than we would in Go.

### Rust
- **Pros:** systems-level control *with* memory safety; excellent for exactly this domain.
- **Cons:** steepest learning curve of all three; the borrow checker plus async ecosystem
  would slow a 10-day timeline the most. Best considered for a future rewrite, not now.

### Python (core)
- **Pros:** fastest to prototype; one language across core + ML.
- **Cons:** the GIL and high-level abstractions hide precisely the concurrency/memory work
  that makes this project impressive. Weakest "systems" signal. Rejected for the core.

## Decision

**Use C++20 for all core services.** We are optimizing for depth of systems understanding
and a distinctive, hard-systems resume signal over raw delivery speed. The concurrency and
memory work is treated as a feature (the point), not a cost.

## Consequences

**Positive**
- We genuinely learn the C++ memory model, RAII, smart pointers, and low-level concurrency.
- The finished artifact is an uncommon, high-signal portfolio piece.
- Forces disciplined interfaces (gRPC/proto) and testing, which improves the design.

**Negative / accepted risks**
- Lower velocity; **Milestone 1 scope is at risk** and may compress (see roadmap's priority
  fallback: Phases 0–2 + minimal search is the non-negotiable demo core).
- More time lost to toolchain and to debugging UB/races than in a managed-runtime language.

## Mitigations

- **Don't hand-roll what isn't the lesson.** Use **RocksDB** for the on-node KV/LSM store
  and **gRPC** for networking — spend our effort on the *distributed* layer above them.
  (See upcoming ADR-0002.)
- **Sanitizers always on** in CI: AddressSanitizer, ThreadSanitizer, UBSan. TSan on every PR
  that touches concurrency is non-negotiable.
- **Modern C++ only:** RAII everywhere, `std::unique_ptr`/`std::shared_ptr`, no raw
  `new`/`delete`, `std::jthread`, `std::atomic`, standard containers. Treat clang-tidy
  warnings as errors.
- **A dependency manager** (vcpkg or Conan — ADR-0003) so we're not fighting builds by hand.
- **Timebox spikes.** If a C++-specific rabbit hole blocks a phase for too long, we log it in
  `progress.md` and route around it rather than sinking the timeline.

## Revisit if

We're materially behind by ~Jul 27 with the demo core (Phases 0–2 + minimal search) still
not working. In that case we reconsider scope first, language second — but a partial C++
system that runs beats a complete one in an easier language for *this* project's goals.

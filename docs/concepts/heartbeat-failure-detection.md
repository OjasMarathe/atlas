# Heartbeats & Failure Detection

**Area:** Fault tolerance · **Phase:** 2 · **Status:** written

## TL;DR

The cluster needs to know which nodes are alive. Atlas's control plane **probes** every storage node
on a timer with a cheap `Heartbeat` RPC and declares a node dead only after **N consecutive** missed
probes; one success revives it. That liveness view is what self-healing consumes.

## The problem it solves

Nothing else in the system can react to a failure until *someone decides* a node is down. But
"down" is not observable — over a network you cannot distinguish a crashed node from a slow one, a
GC pause, or a dropped packet. This is the classic impossibility at the heart of distributed
systems: **failure detection is a guess**, and the design question is how to make a *useful* guess.

Two ways to be wrong:
- **Too eager** — a healthy node with one slow reply gets evicted, triggering pointless
  re-replication (and, in systems with leaders, spurious elections). Flapping costs real I/O.
- **Too patient** — a genuinely dead node keeps being handed reads and the data it held stays
  under-replicated, widening the window in which a second failure loses data.

## How it works

**Push vs pull.** Either nodes send heartbeats to a monitor (push), or the monitor probes them
(pull). Push scales better (each node sends one message regardless of cluster size) and detects
failures sooner. Pull centralizes the decision and needs no new RPC.

**Thresholding.** Rather than "one miss = dead", require `N` *consecutive* failures. Each success
resets the counter. With a per-probe timeout `T` and interval `I`, detection takes roughly
`N × I` and a transient blip of fewer than `N` rounds is absorbed.

**Phi-accrual** (Cassandra/Akka) is the sophisticated alternative: instead of a boolean, output a
*suspicion level* from the statistical distribution of past inter-arrival times, adapting to a link
that is simply slow. Better on heterogeneous networks; more machinery than M1 needs.

## Our implementation in Atlas

- **`src/cluster/health_tracker.{h,cpp}`** — the liveness view. `RecordSuccess`/`RecordFailure` per
  node, a node is dead at `failure_threshold` consecutive failures, one success revives it
  instantly. A node that has **never been probed counts as alive**: it just joined, and treating the
  unprobed as dead would declare a whole fresh cluster dead at startup.
- **`src/cluster/prober.{h,cpp}`** — **pull-based**: the control plane calls the existing
  `StorageService.Heartbeat` on each member with a short deadline and feeds the result to the
  tracker. Pull was chosen because the metadata node is *already* the single authoritative control
  plane ([ADR-0005](../architecture/adr/0005-metadata-single-node-m1.md)), so centralizing liveness
  adds no new failure mode — and it required **no proto change**. Cost is O(nodes) probes per round,
  which is fine at M1 scale; push/gossip is the documented upgrade
  ([ADR-0009](../architecture/adr/0009-failure-detection-and-healing.md)).
- **`ProbeOnce()` is synchronous** — one full round, side effects complete on return. A background
  loop calls it on a timer, and **tests drive detection deterministically instead of sleeping**,
  which is why `health_tracker_test` can kill a node and assert exactly when it is declared dead.

## Complexity & trade-offs

- Per round: `O(nodes)` RPCs; memory `O(nodes)` counters.
- Detection latency ≈ `threshold × interval`; tolerance to blips grows with the same product. That
  single knob is the whole eager/patient trade-off.
- Pull costs the control plane a round-trip per node per round — the reason large clusters push
  or gossip instead.

## Failure modes & edge cases

- **False positive under load** — a saturated but healthy node misses probes and gets declared dead,
  causing needless re-replication. Raise the threshold or the timeout.
- **Asymmetric partition** — a node reachable by some peers and not others. With a single central
  prober the answer is at least *consistent* (one opinion), which is a real benefit of pull.
- **A dead node's data is not gone** — it is unreachable, not wiped. Atlas keeps such a node in the
  chunk location index and filters by liveness at use time, so a returning node is immediately
  useful again.
- **Flapping** — a node oscillating around the threshold triggers repeated healing. Hysteresis
  (a longer revival requirement) is the standard mitigation; not implemented for M1.
- **Everything looks dead** — if the prober itself is partitioned, it declares the whole cluster
  dead. A quorum of observers is the real answer; out of scope for a single-node control plane.

## Alternatives we considered

- **Push heartbeats (node → control plane)** — better scaling and lower detection latency; needs a
  new RPC and leaves the monitor unable to distinguish "node dead" from "node silent". Upgrade path.
- **Gossip (SWIM)** — no central monitor, scales to very large clusters, tolerates a dead monitor;
  substantially more machinery, and pointless while the control plane is deliberately single-node.
- **Phi-accrual** — adaptive suspicion rather than a fixed threshold; the right call on noisy or
  heterogeneous networks.
- **TCP connection state alone** — cheap but wrong: a live connection says nothing about whether the
  process is making progress.

## Interview Q&A

**Q: Why can't you know a node is down?** You can't distinguish crashed from slow over a network —
detection is a timeout-based guess, so you tune how wrong you're willing to be in each direction.

**Q: Why require consecutive failures?** One miss is usually a blip; evicting on it causes flapping
and pointless re-replication. Consecutive misses make the signal robust at the cost of latency.

**Q: Push or pull, and why did you pull?** Push scales better; we pull because the control plane is
already single and authoritative, it keeps one consistent opinion of liveness, and it needed no new
RPC. Push/gossip is the upgrade when node count grows.

**Q: What is phi-accrual?** A detector that outputs a continuous suspicion level from the observed
distribution of heartbeat inter-arrival times, instead of a fixed timeout — it adapts to a link
that's merely slow.

**Q: What happens to a node that comes back?** One successful probe revives it. Its chunks were
never deleted from the location index, so it is immediately a usable replica again.

## References

- Chandra & Toueg, *Unreliable Failure Detectors for Reliable Distributed Systems* (1996).
- Hayashibara et al., *The φ Accrual Failure Detector* (2004).
- Das et al., *SWIM: Scalable Weakly-consistent Infection-style Process Group Membership* (2002).

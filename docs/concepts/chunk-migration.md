# Chunk Migration (rebalancing on node join)

**Area:** DFS / fault tolerance · **Phase:** 2 · **Status:** written

## TL;DR

When a node joins, the ring reassigns a slice of the keyspace to it — but the chunks in that slice
are still sitting, fully replicated, on the old nodes. **Migration** moves them: copy each affected
replica onto its new rightful node, then drop the copy that no longer belongs. Measured on a 5→6
node cluster, **17.5% of replicas moved** — almost exactly the ideal 1/6.

## The problem it solves

Self-healing reacts to *missing* copies. A node join creates no missing copies at all: every chunk
still has R live holders, so [the healer](self-healing.md) correctly ignores them — and the new node
would stay **empty forever**. Two things go wrong if you leave it:

- **No load sharing.** The point of adding a node is to absorb data and traffic. An empty node
  absorbs neither.
- **Placement drifts from the ring.** Readers and the healer compute intended placement from the
  ring; the further actual placement drifts, the less that computation means.

So the trigger is different from repair: not *"fewer than R live holders"* but *"the ring now wants
this chunk somewhere it isn't."*

## How it works

```
for each chunk:
    live = holders ∩ alive
    if |live| < R:  skip            ← degraded: repair owns it, durability first
    ideal   = ring.Replicas(chunk_id, R)     ← where it belongs now
    wanted  = ideal nodes that are alive and not already holders
    if wanted empty: skip                     ← already correctly placed
    data    = GetChunk(any live holder)
    surplus = live holders NOT in ideal        ← eviction candidates
    for target in wanted:
        ReplicateChunk(target); record holder     ← COPY FIRST
        if surplus: DeleteChunk(surplus.pop()); drop holder   ← evict only after
```

Three rules carry the safety of this:

1. **Copy before evict, never the reverse.** Momentarily holding R+1 copies is free; momentarily
   holding R−1 is a durability hole opened voluntarily, for cosmetic reasons.
2. **Skip degraded chunks entirely.** If a chunk is already short a copy, moving its replicas around
   is precisely when a mistake is least affordable. Repair runs first and fixes it; the next
   rebalance pass picks it up.
3. **Evict only holders outside the ideal set.** They're the ones the ring no longer wants, so the
   count lands back at exactly R.

### Why only ~1/N moves

That's the consistent-hashing property ([consistent-hashing.md](consistent-hashing.md)) observed at
the *data* level rather than the ring level. Adding the (N+1)th node makes it the new owner of one
arc per vnode; every chunk landing in those arcs gains it as a replica and sheds one other. Total
replica movement ≈ `C·R / (N+1)` out of `C·R` — i.e. **1/(N+1)**. With `hash % N` placement, changing
N reshuffles essentially *every* chunk, which is exactly the failure mode consistent hashing exists
to avoid.

## Our implementation in Atlas

- **`Healer::RebalanceOnce(ring_state)`** in `src/cluster/healer.cpp`, returning a
  `RebalanceReport{chunks_scanned, misplaced, migrated, evicted, skipped_degraded}`. It shares the
  healer's plumbing (ring, liveness, stub cache, chunk transfer) because the two operations differ
  only in trigger, not in mechanics.
- Runs in the [maintenance loop](../architecture/adr/0009-failure-detection-and-healing.md) *after*
  repair each round (`MaintenanceOptions::rebalance`), so durability work always precedes tidiness.
- Synchronous and idempotent, so a pass over a balanced cluster is a no-op and it converges.
- **Verified** in `tests/rebalance_test.cpp` on a 5→6 node cluster with 40 distinct chunks:
  `21/120 replicas moved (17.5%)`, every move landed on the newcomer (zero churn between existing
  nodes), every chunk kept exactly R holders throughout, and the file downloaded byte-identical
  afterwards.

## Complexity & trade-offs

- A pass is `O(chunks)` metadata reads; each migration is one chunk transfer plus a delete.
- **Bytes flow through the control plane** (pull from a holder, push to the target) — same
  limitation as repair; a source-driven push RPC is the fix.
- **No rate limiting.** Adding a node to a large cluster triggers all its migrations at once. Real
  systems throttle rebalancing precisely because it competes with live traffic.
- Full-scan per pass doesn't scale; a per-node chunk list would let us touch only affected chunks.

## Failure modes & edge cases

- **Join during a failure** — degraded chunks are skipped, so a join while a node is down doesn't
  compound the problem; repair goes first.
- **Migration interrupted midway** — a chunk may briefly hold R+1 copies. Harmless (extra
  durability), and the next pass evicts the surplus.
- **Target write succeeds, delete fails** — the chunk stays at R+1 and the stale holder remains
  recorded; retried next pass, or reclaimed by GC.
- **Node joins and leaves repeatedly** causes migration churn — the same flapping problem failure
  detection has, and the same answer (hysteresis / a settling delay before rebalancing).
- **A node joining an empty cluster** does nothing: no chunks, nothing misplaced.

## Alternatives we considered

- **Never migrate; only place new writes by the current ring** — trivially simple, and the new node
  fills up only as new data arrives. Existing data stays skewed forever; acceptable for
  append-heavy workloads, wrong as a general answer.
- **Migrate on read** (move a chunk when someone touches it) — spreads cost over time and only
  moves data anyone actually wants, but cold data never migrates.
- **Full reshuffle to ideal placement** — recompute everything and move whatever's wrong. Correct
  and much simpler to reason about, but it discards the 1/N property that makes joins cheap.
- **Virtual-node reassignment without data movement** (redirect reads to the old holder) — avoids
  the copy at the cost of a permanent indirection layer.

## Interview Q&A

**Q: A node joins and nothing is under-replicated. Why does anything need to happen?** Because the
ring reassigned part of the keyspace to it. Without migration the new node stays empty — no load
sharing — and actual placement drifts from what the ring says, which is what everything else
computes against.

**Q: How much data should move, and why?** About `1/(N+1)` of all replicas — just the new node's
share. That's the defining property of consistent hashing; `hash % N` would move nearly everything.
We measured 17.5% going 5→6 nodes.

**Q: Copy-then-delete or delete-then-copy?** Always copy first. Dropping a replica before its
replacement is durable opens a durability hole voluntarily, to satisfy a placement preference.

**Q: What if a chunk is under-replicated when rebalancing runs?** Skip it. Repair restores
durability first; tidiness can wait a round.

**Q: What's the danger of rebalancing a big cluster?** A migration storm — every affected chunk
moves at once, competing with live traffic. Production systems rate-limit it.

## References

- Ghemawat et al., *The Google File System* (SOSP 2003) — rebalancing and chunk placement.
- DeCandia et al., *Dynamo* (SOSP 2007) — node join/leave and partition handoff.
- [consistent-hashing.md](consistent-hashing.md) — why only 1/N moves.

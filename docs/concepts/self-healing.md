# Self-Healing (re-replication)

**Area:** Fault tolerance · **Phase:** 2 · **Status:** written

## TL;DR

When a node dies, every chunk it held drops to 2 live copies. The **healer** scans the chunk
location index, finds chunks below the replication factor, copies the bytes from a survivor onto a
fresh ring-chosen live node, and records the new holder. That is the difference between *"the read
worked because we routed around the failure"* and *"the cluster is genuinely back to 3 copies."*

## The problem it solves

Phase 1 gave us read-around: a dead replica doesn't break reads. But it leaves the system
**silently degraded** — the chunk now has 2 copies and nothing will ever restore the third. Failures
accumulate, and the window in which a *second* failure destroys data stays open forever. Durability
is not a property you have once at write time; it's one you must continuously restore.

## How it works

```
for each chunk:
    live_holders = recorded holders ∩ alive
    if |live_holders| >= R:            healthy, skip
    if live_holders is empty:          unrepairable — every copy is gone
    data = GetChunk(any live holder)   ← content-addressed: every replica is equally authoritative
    for candidate in ring.preference_order(chunk_id):
        stop when |holders| == R
        skip if already a holder, dead, or not in the membership
        ReplicateChunk(candidate, data); record the new holder
```

Three details carry the design:

1. **The location index is the input.** The healer's whole job is comparing *intended* placement
   (`ring.Replicas(chunk_id, R)`) with *actual* holders. That's only possible because Upload records
   **only nodes that acked** — recording every intended replica would make an under-replicated chunk
   look permanently healthy and the healer would never fire. (Exactly the bug caught in PR #5 review.)
2. **Any survivor can serve the copy.** Chunks are immutable and content-addressed
   ([ADR-0004](../architecture/adr/0004-replication-consistency.md)), so there is no primary to
   consult and no reconciliation — the healer picks whichever live replica answers first.
3. **Repair is idempotent and convergent.** A second pass over a healthy cluster does nothing, so
   the loop can run forever on a timer without accumulating work or double-repairing.

### Why "replica promotion" doesn't exist here

The roadmap lists replica promotion, and in GFS-style systems it's real: a chunk's *primary* holds a
lease and serializes mutations, so its death requires electing a successor. **Atlas has no such
role** — chunks never mutate, so there is nothing to serialize and every live replica is already
authoritative for reads. Promotion is degenerate by construction. That's a consequence of the
immutability decision in ADR-0004, and worth stating rather than inventing a ceremonial election.

## Our implementation in Atlas

- **`src/cluster/healer.{h,cpp}`** — `RepairOnce(ring_state)` does one synchronous pass and returns a
  `HealReport{chunks_scanned, under_replicated, repaired, unrepairable}`. Synchronous so a timer
  loop can call it *and* tests can call it directly.
- **Location index** (`src/metadata/metadata_store.cpp`, `c/<chunk_id>` → `ChunkPlacement`) — the
  mutable truth about who holds a chunk, deliberately separate from the immutable file versions, so
  healing updates placement without rewriting history. `GetFile` serves the *live* index, so readers
  see repaired placements with **no client change at all**.
- **Verified** in `tests/self_healing_test.cpp`: upload → kill a holder → two missed probes mark it
  dead → `RepairOnce` reports exactly 1 repair → the chunk gains a holder outside the original three,
  that node really serves the bytes, live holders are back to 3, a second pass is a no-op, and the
  file still downloads.

## Complexity & trade-offs

- A pass is `O(chunks)` metadata reads; repair costs one chunk transfer per missing replica.
- **Bytes flow through the healer** (pull from a survivor, push to the target), so the control plane
  carries repair traffic. A source-driven push RPC ("node X, send chunk C to node Y") would keep
  bytes on the data plane — the documented upgrade.
- Full scans don't scale to millions of chunks; production systems keep a per-node chunk list and
  repair only what the dead node held.

## Failure modes & edge cases

- **All replicas gone** — unrepairable, and counted as such. Nothing can synthesize lost bytes; this
  is the case replication exists to make unlikely, not impossible.
- **A returning node causes over-replication** — its copies were never deleted, so a healed chunk can
  end up with 4 holders. Benign (extra durability); reclaiming the surplus is GC's job.
- **Repair storms** — losing a large node makes every chunk it held under-replicated at once,
  saturating the network exactly when the cluster is already degraded. Real systems rate-limit
  repair; not implemented for M1.
- **Flapping nodes** cause repeated repair — mitigated by the failure-detection threshold, see
  [heartbeat-failure-detection.md](heartbeat-failure-detection.md).
- **Healing onto a dead node** is prevented explicitly: candidates are filtered by liveness.

## Alternatives we considered

- **Repair on read** (fix a chunk only when someone reads it) — cheap and lazy, but cold data stays
  degraded indefinitely, which is precisely the data you can least afford to lose.
- **Per-node chunk lists** (repair only what the dead node held) — far better than a full scan and
  the obvious next step; needs a reverse index (node → chunks).
- **Erasure-coded repair** — reconstruct from parity instead of copying; much less storage overhead,
  much more repair CPU and network amplification.
- **Anti-entropy / Merkle-tree sync** (Dynamo) — replicas periodically compare hash trees and
  reconcile differences. Powerful for divergent *mutable* data; unnecessary when chunks are immutable
  and identity is the hash.

## Interview Q&A

**Q: Why isn't read-around enough?** It masks a failure without fixing it — the chunk stays at 2
copies forever, so failures accumulate and the second one loses data. Durability must be restored,
not merely tolerated.

**Q: How does the healer know a chunk is under-replicated?** It compares recorded holders (the
location index) against liveness and the ring's intended placement. This only works because writes
record *acked* holders, not intended ones.

**Q: Which replica does it copy from, and how does it choose?** Any live one — chunks are immutable
and content-addressed, so all replicas are identical and equally authoritative; the checksum on read
proves it got the right bytes.

**Q: Where's the primary you promote?** There isn't one. Immutable chunks need no write
serialization, so every live replica is already authoritative and promotion is degenerate.

**Q: What's a repair storm and how would you prevent it?** Losing a big node makes everything it held
under-replicated simultaneously, saturating the network while degraded. Rate-limit repairs and
prioritize the most-degraded chunks (2 copies missing before 1).

## References

- Ghemawat et al., *The Google File System* (SOSP 2003) — re-replication and rebalancing.
- DeCandia et al., *Dynamo* (SOSP 2007) — hinted handoff and anti-entropy as alternatives.
- [ADR-0009](../architecture/adr/0009-failure-detection-and-healing.md) — the Phase 2 decisions.

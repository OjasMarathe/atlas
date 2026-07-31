# Replication (3× with a 2-of-3 write quorum)

**Area:** DFS · **Phase:** 1 · **Status:** written

## TL;DR

Every chunk is stored on **3 distinct nodes** chosen by the consistent-hashing ring. A write is
acked once **2 of the 3** have it durably (W=2); a read fetches from any healthy replica, verifies
the checksum, and **reads around** a dead one. This survives one node failure with zero data loss
and no write stall.

## The problem it solves

A single copy of a chunk is one disk failure from data loss and one reboot from unavailability.
Replication trades storage (3×) for durability + availability. The design questions: *where* do the
copies go, *when* is a write done, and how do reads cope with a dead copy.

## How it works

- **Placement** — `ring.replicas(chunk_id, 3)` walks the ring clockwise to 3 distinct physical nodes
  (primary/secondary/tertiary; see [consistent-hashing.md](consistent-hashing.md)). Deterministic,
  so any client computes the same placement from the same membership.
- **Write (W=2)** — the ingestion client writes the chunk to all 3 replicas and acks once **≥2**
  succeed. The 3rd may lag (repaired by Phase 2 self-healing). W=2 survives one node down without
  stalling on a slow node.
- **Read (R=1) + read-around** — download reads each chunk from the first healthy replica in its
  placement list; on failure (dead node or checksum mismatch) it tries the next. Any replica is
  authoritative because chunks are immutable + content-addressed ([ADR-0004](../architecture/adr/0004-replication-consistency.md)) — no read quorum needed.
- **Commit point** — the file "exists" once the metadata service records its chunk placements, after
  the chunks reach W=2.

## Our implementation in Atlas

- `src/client/client.cpp` — **Upload**: chunk → `ring.replicas` → `PutChunk` to each → record **only
  the nodes that acked** → require W=2 → `RegisterFile`. **Download**: `GetFile` → for each chunk,
  `GetChunk` from the recorded holders in order until one succeeds (read-around) → reassemble →
  verify whole-file SHA-256.
- **Client-driven fan-out for M1**: the client writes to all 3 nodes directly. The proto has a
  `ReplicateChunk` RPC for primary-driven fan-out (primary pushes to its secondaries), which halves
  client bandwidth — a natural refinement, deferred.
- **Verified end-to-end** in `tests/phase1_e2e_test.cpp`: a chunk lands on 3 distinct nodes (confirmed
  by direct `GetChunk`), download reassembles, and — the headline — **killing a replica still serves
  the file via read-around**.

## Complexity & trade-offs

- Write: R `PutChunk`s per chunk (client-driven → R× client bandwidth). Read: 1 `GetChunk` (R=1),
  plus one per dead replica encountered.
- **W=2 / R=1 / N=3**: `R+W = N` (not `> N`), which is safe here *only* because chunks are immutable
  ([ADR-0004](../architecture/adr/0004-replication-consistency.md)) — there are no conflicting writes
  to reconcile.
- Accepted risk: a W=2 write loses data only on a simultaneous double failure before the 3rd copy
  exists — out of scope for M1; Phase 2 self-healing shrinks the window.

## Failure modes & edge cases

- **Fewer than R nodes** — Upload rejects (can't meet the replication factor).
- **One replica down at write** — still acks on the other 2 (W=2), and the placement records **only
  the 2 that acked**. Recording all 3 *intended* replicas would make the chunk look permanently
  healthy and starve Phase 2's healer of its input.
- **One replica down at read** — read-around serves from a survivor. If *all* replicas of a chunk are
  down, the read fails (nothing healthy to serve).
- **Dead node still in membership** — for M1 a killed node stays in the ring (failure detection is
  Phase 2); read-around tolerates it, and a fail-fast RPC deadline keeps a dead replica from stalling
  the read.
- **Under-replication is visible but unrepaired in M1** — the placement list is the truth about who
  holds a chunk, so comparing it against `ring.Replicas(chunk_id, R)` (intended vs actual) detects a
  missing copy. Nothing yet *acts* on that difference; re-replication is Phase 2.

## Alternatives we considered

- **Primary-driven replication** (primary fans out via `ReplicateChunk`) — less client bandwidth,
  more coupling; the proto supports it, deferred.
- **W=3 (write all)** — strongest durability, but one slow node stalls every write. W=2 is the balance.
- **W=1 (ack on primary)** — fastest, but losing the primary right after acking loses data. Rejected.
- **Erasure coding** (Reed–Solomon) — far less storage overhead than 3× for the same durability, at
  the cost of CPU + reconstruction complexity; the right choice at scale, overkill for M1.

## Interview Q&A

**Q: Why W=2/R=1 with N=3?** Survives one failure with no write stall (W=2) and fast reads (R=1);
safe despite `R+W=N` because chunks are immutable — no write conflicts.
**Q: What is read-around?** On a chunk read, try replicas in order until one returns checksum-valid
bytes — tolerates a dead replica without failing the read.
**Q: A write reaches only 1 node — then what?** Rejected (below W=2); the client doesn't commit the file.
**Q: How is a lost replica restored?** Not in M1 (under-replication is silent); Phase 2 self-healing
re-replicates to restore the factor.
**Q: 3× storage is expensive — alternative?** Erasure coding gives comparable durability at a fraction
of the overhead, trading CPU and reconstruction complexity.

## References

- DeCandia et al., *Dynamo* (SOSP 2007) — N/R/W quorum replication.
- Ghemawat et al., *The Google File System* (SOSP 2003) — chunk replication + primary/replica.
- [ADR-0004](../architecture/adr/0004-replication-consistency.md) — the W=2/R=1 decision and the immutability insight.

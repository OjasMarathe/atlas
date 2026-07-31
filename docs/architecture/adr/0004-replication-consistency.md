# ADR-0004 — Replication & consistency model

**Status:** Accepted (2026-07-27)

## Context

Phase 1's distributed file system stores every chunk on 3 nodes. We must pin down: the
replication factor, placement, when a write is acknowledged, how reads work, and the resulting
consistency guarantees.

## Key enabler: chunks are immutable and content-addressed

A chunk's id is the hex SHA-256 of its bytes, and chunk bytes never change — an edit produces
*new* chunks and a *new* file version (copy-on-write), never an in-place mutation. This removes
the classic distributed-storage hazard of concurrent conflicting writes to the same data: a given
`chunk_id` either matches its bytes or it doesn't. **All the genuinely hard consistency is
therefore confined to the metadata** (the file → chunk → node map + versions), which lives on the
single authoritative metadata node ([ADR-0005](0005-metadata-single-node-m1.md)).

## Decision

- **Replication factor N = 3**, placed on 3 distinct physical nodes by walking the
  consistent-hashing ring (primary, secondary, tertiary — see
  [consistent-hashing.md](../../concepts/consistent-hashing.md)).
- **Write ack at W = 2.** A chunk write returns success once the **primary + at least one replica**
  have durably stored it (WAL fsync). The 3rd replica is filled asynchronously (and repaired by
  Phase 2 self-healing if it lags).
- **Read at R = 1.** Read from any healthy replica and verify the checksum; because chunks are
  immutable + content-addressed, any replica holding the `chunk_id` is authoritative — no read
  quorum needed.
- **Commit point = metadata.** A file "exists" once the metadata service records its
  `FileMetadata`, which happens only after its chunks reach W = 2 durability.

## Consequences

- **Survives one node failure with zero data loss**, and no write stalls on a single slow/dead node
  (unlike W = 3).
- **Fast reads** (R = 1) with integrity guaranteed by the checksum, not by a quorum.
- **Accepted risk:** a write acked at W = 2 loses data only if *both* holders die before the 3rd
  copy is made — a simultaneous double failure, out of scope for M1. Phase 2 self-healing shrinks
  that window by promptly restoring the 3rd copy.
- **Strong consistency where it matters:** metadata is strongly consistent (single authoritative
  node); chunk data is immutable + verifiable. We do not implement multi-writer conflict resolution
  because the design makes it unnecessary.
- This is the classic tunable-quorum balance (Dynamo/Cassandra). Normally strong consistency needs
  `R + W > N`; here `R + W = N`, but **immutability lets us relax that rule for chunk data** — a
  nice, defensible design insight.

## Revisit if

Metadata becomes replicated via Raft (post-M1): metadata then gets its own consensus, and this ADR
governs only chunk data.

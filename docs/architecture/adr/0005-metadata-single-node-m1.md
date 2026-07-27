# ADR-0005 — Metadata service: single node for Milestone 1

**Status:** Accepted for M1 (2026-07-21) — expected to be superseded post-M1 by a Raft-backed design

## Context

The metadata service is the authoritative control plane: it owns the file→chunk→node map, the
consistent-hashing ring/membership, and versions. Making it highly available (replicated via
Raft) is the "correct" design but a large build. For [Milestone 1](../../plan/roadmap.md) we
optimize for the end-to-end demo (upload → replicate → distributed search → self-heal).

## Options considered

- **Single metadata node** — simplest; a single point of failure (SPOF) for the control plane.
- **Raft-replicated metadata** — highly available, no SPOF; significant implementation cost
  (leader election + replicated log + snapshotting).
- **Gossip / eventually-consistent metadata** — wrong for *authoritative* data; risks divergent
  chunk maps.

## Decision

**A single metadata node for M1**, persisting its state to RocksDB + WAL so it **recovers on
restart**. It is an explicit, documented SPOF. Crucially, we design the `MetadataService` proto
contract so callers are agnostic to whether it's single-node or Raft-replicated — the upgrade is
a drop-in later.

## Consequences

- **Positive:** dramatically less to build for M1; the demo path works end-to-end.
- **Negative / accepted risk:** if the metadata node dies, the **control plane** is unavailable
  until it restarts. Note the blast radius is limited — **chunk data on storage nodes is safe and
  replicated 3×**; only the *map* is temporarily unavailable, and it recovers from its WAL.
- **Mitigations:** persist to RocksDB+WAL for fast recovery; keep the proto stable so Phase 2's
  Raft stretch (or an early post-M1 upgrade) requires no caller changes.
- **Interview value:** "single node for M1, here's exactly how I'd make it HA with Raft, and
  here's why the data itself was never at risk" is a strong, honest talking point.

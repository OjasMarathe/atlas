# ADR-0009 — Failure detection & self-healing model

**Status:** Accepted (2026-07-29)

> ADR-0008 is reserved for the query-parsing-location decision raised in Harshal's Phase-3 work
> (see the ADR index), so Phase 2 takes 0009.

## Context

Phase 1 leaves the cluster able to *tolerate* a dead node (read-around) but not to *recover* from
one: a chunk that drops to 2 copies stays there forever. Phase 2 must decide (a) how liveness is
determined, (b) where the "who holds this chunk" truth lives so healing can update it, and (c) what
"replica promotion" means in a store of immutable chunks.

## Decisions

### 1. Failure detection is **pull-based**, with a consecutive-failure threshold

The control plane probes each storage node with the existing `StorageService.Heartbeat` RPC; a node
is declared dead after N consecutive missed probes and revived by a single success.

- **Why pull:** the metadata node is already the single authoritative control plane
  ([ADR-0005](0005-metadata-single-node-m1.md)), so centralizing liveness introduces no new failure
  mode and yields one consistent opinion. It also required **no proto change**.
- **Why a threshold:** one missed probe is a blip, not a death; evicting on it causes flapping and
  pointless repair traffic.
- **Cost:** O(nodes) RPCs per round. Fine at M1 scale; push or gossip (SWIM) is the upgrade when
  node count grows.

### 2. Chunk locations live in a **mutable index**, separate from immutable file versions

`c/<chunk_id> → ChunkPlacement` in the metadata store, written in the same atomic `WriteBatch` as a
file's version commit, and merged (union) rather than overwritten.

- A file *version* is immutable history; *where its chunks are* is not — healing changes it. Keeping
  locations in the version blob would force rewriting immutable history on every repair.
- Content-addressed chunks are shared across files and versions, so a chunk→holders index dedups
  naturally: one entry regardless of how many versions reference it.
- **`GetFile` serves the live index**, so readers automatically see repaired placements — the
  ingestion/read client needed **no change at all**.
- Consequence: a version blob's recorded placements are a historical snapshot, not the read path.

### 3. A dead node is **not removed** from the location index

Its bytes are unreachable, not deleted. Liveness is applied at *use* time (the healer filters,
readers route around), so a node that returns is immediately a useful replica again.

- Consequence: a returning node can leave a chunk with R+1 holders. Benign extra durability;
  reclaiming the surplus is GC's job.

### 4. **Replica promotion is degenerate** and deliberately not implemented

In GFS a chunk's primary holds a lease and serializes mutations, so its death requires electing a
successor. Atlas chunks are immutable and content-addressed
([ADR-0004](0004-replication-consistency.md)): there is nothing to serialize, and every live replica
is already authoritative for reads. We document this rather than implement a ceremonial election.

### 5. The healer moves bytes **through the control plane** for M1

`RepairOnce` pulls a chunk from a live holder and pushes it to the target. Simple and uses only
existing RPCs, at the cost of routing repair traffic through the metadata node. A source-driven
push RPC (`"node X, send chunk C to node Y"`) is the documented upgrade; it needs a proto addition
and therefore joint review.

## Consequences

**Positive**
- Under-replication is now both *visible* (index vs ring) and *repaired*; durability is continuously
  restored rather than merely tolerated.
- No client changes and no proto changes for the whole of Phase 2's core.
- Probing and healing are synchronous `…Once()` calls, so tests drive them deterministically instead
  of sleeping — the reason `self_healing_test` can assert *exactly* when a node is declared dead and
  exactly how many replicas were created.

**Negative / accepted**
- Full-scan repair is `O(chunks)` per pass — needs a per-node chunk list to scale.
- Repair traffic crosses the control plane.
- No repair rate-limiting: losing a large node makes everything it held under-replicated at once.
- No hysteresis for flapping nodes beyond the failure threshold.

## Revisit if

Node count or chunk count grows enough that O(nodes) probing or O(chunks) scanning dominates, or
when metadata becomes Raft-replicated (liveness would then need to be a replicated decision, not a
single node's opinion).

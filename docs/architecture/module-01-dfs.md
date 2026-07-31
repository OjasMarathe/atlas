# Module 1 — Distributed File System (Phase 1 design)

**Status:** design · **Milestone:** M1 · **Owner:** Ojas (storage spine)

## Goal & definition of done

Upload a file → it's split into 4 MB chunks, each SHA-256-fingerprinted, checksummed, versioned,
and stored on 3 nodes → reads verify the fingerprint → adding a 4th node migrates only its fair
share of chunks. *(Failure detection / self-healing is Phase 2.)*

## Design principles

1. **Content-addressed, immutable chunks** — `chunk_id = hex SHA-256(bytes)`; chunks never mutate.
   Edits are copy-on-write → new chunks + a new file version. This is the keystone
   ([ADR-0004](adr/0004-replication-consistency.md)): it makes chunk consistency trivial and dedups
   identical chunks for free.
2. **Metadata separate from data** — the file→chunk→node map lives in the metadata service; we
   never scan disks to find data.
3. **Placement by the ring** — the consistent-hashing ring decides which nodes hold a chunk; a
   node join/leave moves only a fraction of chunks
   ([consistent-hashing.md](../concepts/consistent-hashing.md)).
4. **3× replication, W = 2 ack** ([ADR-0004](adr/0004-replication-consistency.md)).

## Components (Phase 1)

| Component | Role | Backing |
|---|---|---|
| **Ingestion** (stateless) | the upload pipeline: chunk → place → replicate → register → (index) | — |
| **Metadata service** (single node) | `FileMetadata` store + the ring/membership | RocksDB |
| **Storage nodes** | chunk put/get/delete + replicate + checksum | RocksDB + WAL |
| **Hash ring** (`src/common/hash_ring`) | placement; owned by metadata, consumed by ingestion | in-memory + persisted |

## Data model

- **Chunk** — fixed 4 MB (last chunk smaller). `chunk_id = hex SHA-256(bytes)`, immutable. Stored
  key→value in RocksDB on each holder (`key = chunk_id`, `value = bytes`); checksum verified on read.
- **FileMetadata (per version)** — `file_id, owner, version, ordered [ChunkPlacement],
  replication_factor, sha256(whole file), created_at, modified_at` (proto `FileMetadata`).
  `ChunkPlacement = {ChunkHandle(chunk_id, size), [primary, secondary, tertiary] node_ids}`.
- **Versioning (copy-on-write)** — re-uploading creates a *new* `FileMetadata` with `version+1` and
  a new chunk list. Unchanged chunks are shared automatically (same content → same `chunk_id` → same
  stored bytes). Old versions retained until GC.

## Write path (upload)

```
client → Ingestion.Upload(file)
 1. split file into 4 MB chunks
 2. for each chunk: chunk_id = SHA-256(bytes)
 3.   placements = ring.replicas(chunk_id, 3)            // [primary, secondary, tertiary]
 4.   StorageService.PutChunk → primary                  // primary WAL-fsyncs, stores, verifies id
 5.   primary → ReplicateChunk → secondary & tertiary
 6.   ack the chunk once W=2 (primary + ≥1 replica) are durable
 7. sha256_file = SHA-256(whole file)
 8. MetadataService.RegisterFile{file_id, chunks:[placements], version, sha256_file}   ← COMMIT POINT
 9. (later) hand extracted text to SearchService.IndexDocument
```

A file "exists" only after step 8 (the metadata commit).

## Read path (download)

```
client → MetadataService.GetFile(file_id, version=latest)
 for each chunk in order:
   pick a healthy replica from placement.node_ids
   StorageService.GetChunk(chunk_id, verify_checksum=true)
   if checksum mismatch → try the next replica
 reassemble chunks → file; optionally verify sha256_file
```

## Storage engine (per node)

- **RocksDB** ([ADR-0002](adr/0002-storage-engine-rocksdb.md)) as the local KV store; column
  families separate chunk data from bookkeeping.
- **WAL** — append the write intent before storing, so a crash mid-write is recoverable. (RocksDB
  has its own WAL; we still implement + document the concept — `concepts/wal.md`.)
- **Checksum on read** — recompute SHA-256, compare to `chunk_id`; mismatch ⇒ corruption ⇒ read
  another replica (`concepts/sha256-checksums.md`).

## Consistent hashing & migration

- The **metadata service owns the ring** (versioned `RingState`) and serves it to ingestion.
- **Placement:** `ring.replicas(chunk_id, 3)` → 3 distinct physical nodes (skipping vnodes on
  already-chosen nodes — see the concept note).
- **Node join:** insert the new node's vnodes → only chunks whose ring-successor changed migrate to
  it. Phase 1 does a straightforward "recompute placements for affected chunks and copy them over";
  fancier rebalancing is a stretch.

## Garbage collection (basic)

A chunk is live if referenced by any non-GC'd `FileMetadata` version. GC = mark-and-sweep (or
refcount) over metadata → delete unreferenced chunks from storage nodes (`concepts/garbage-collection.md`).
Phase 1: a triggered GC pass; automation later.

## Failure handling — Phase 1 vs Phase 2

- **Phase 1 (this module):** happy path + W = 2 durability + checksum-verified reads + read-around a
  dead replica. If a node is down at write time, place on the next ring node.
- **Phase 2 (next):** heartbeats, failure detection, replica promotion, and self-healing
  (re-replicate under-replicated chunks back to 3). We design the interfaces now so Phase 2 slots in.

## Build order (each piece ships with its concept note + tests)

1. **Chunking + content addressing** — split + SHA-256; roundtrip test · *chunking, sha256-checksums*
2. **Storage node (single)** — `PutChunk`/`GetChunk`/`DeleteChunk` on RocksDB + WAL + checksum-on-read · *wal*
3. **Metadata service (single)** — `RegisterFile`/`GetFile`/`ListVersions` on RocksDB; versioning · *metadata, versioning*
4. **Hash ring** — `src/common/hash_ring`; placement (concept note already written ✅)
5. **Replication** — `PutChunk` fans out to 3; W = 2 ack; read-around · *replication*
6. **Node join + migration** — add a node, migrate its share
7. **Ingestion path** — end-to-end `Upload` wiring (the write path above)

## Interfaces

`StorageService` + `MetadataService` in `proto/` are already contracted. Extend via PR if needed
(stable field numbers).

## Testing plan

- **Unit:** chunking roundtrip; SHA-256 corruption detection; ring placement (3 distinct nodes,
  migration fraction); metadata versioning.
- **Integration** (multi-node via docker-compose): upload → chunks land on 3 nodes → download
  verifies; kill one storage node → read still succeeds from a replica (self-heal is Phase 2).

## Concept notes owed

`chunking`, `sha256-checksums`, `wal`, `metadata` (+ `versioning`), `replication`,
`garbage-collection`. (`consistent-hashing` already written ✅.)

# Write-Ahead Log (WAL)

**Area:** DFS / durability · **Phase:** 1 · **Status:** written

## TL;DR

A WAL makes writes crash-safe: before touching the real data structure, you first **append** a
record of the intended change to an append-only log and **fsync** it. If the process crashes
mid-write, you replay the log on restart. Atlas gets this from RocksDB's built-in WAL (via sync
writes) — it's what makes a chunk durable *before* we ack it.

## The problem it solves

A write usually isn't atomic — updating an on-disk structure (an LSM memtable flush, a B-tree node)
takes several steps, and a crash in the middle can leave it half-updated and corrupt. The WAL turns
"modify the complex structure" into "first append one durable record," which *is* effectively atomic
(append + fsync), so you can always recover a consistent state.

## How it works

1. To apply change X: append "X" to the append-only WAL and **fsync** (force it to disk).
2. Only then apply X to the main structure (in memory, flushed to disk lazily).
3. On restart after a crash: **replay** WAL records not yet reflected in the main structure.
4. Once the main structure is durably past a record, the WAL is truncated/rotated (checkpointing).

The WAL write is sequential (fast) and the fsync makes the change durable *before* you tell the
client "done" — so a crash never loses an acked write.

## Our implementation in Atlas

- `src/storage/chunk_store.cpp` writes with `WriteOptions::sync = true`, which appends to RocksDB's
  WAL and fsyncs before returning. So `ChunkStore::Put` is durable when it returns — which is exactly
  what [ADR-0004](../architecture/adr/0004-replication-consistency.md)'s W=2 ack depends on ("a chunk
  is durable on a node once its Put returns").
- We rely on **RocksDB's** WAL rather than hand-rolling one ([ADR-0002](../architecture/adr/0002-storage-engine-rocksdb.md):
  buy the storage engine) — but we document + understand it because it's the mechanism behind our
  durability guarantee. Exercised by the persistence test (`chunk_store_test`: data survives a
  close + reopen).

## Complexity & trade-offs

- **Sync writes** (fsync per write) are durable but slower than buffered writes. `sync=false` batches
  for throughput at the risk of losing the last few writes on a crash. We choose `sync=true` for
  chunk durability; batched writes are a future throughput knob.
- **Double write:** data is written twice (WAL, then the LSM). The WAL is sequential so it's cheap —
  worth it for crash safety.

## Failure modes & edge cases

- **fsync can lie** — some disks/OSes buffer even after fsync; true durability needs honest hardware.
- **WAL growth** — without checkpointing the log grows unbounded; RocksDB flushes memtables + rotates
  the WAL to bound it.
- **Torn tail record** — a half-written record at the WAL tail is caught by per-record checksums and
  discarded on replay.

## Alternatives we considered

- **Write in place, no WAL** — fast but a mid-write crash risks corruption. Rejected.
- **Copy-on-write / shadow paging** — write new pages, then atomically flip a pointer (LMDB-style);
  different trade-offs. RocksDB uses WAL + LSM, so we do too.
- **Hand-rolled WAL** — educational but redundant given RocksDB; we document the concept instead.

## Interview Q&A

**Q: Why write the data twice?** The WAL append is atomic + sequential (cheap), giving crash safety
the complex main structure can't guarantee mid-update.
**Q: What does `sync=true` buy you?** The write is fsynced before returning, so an acked write
survives a crash — the basis of our durability guarantee.
**Q: WAL vs shadow paging?** Both give crash safety; WAL logs intentions then applies; shadow paging
writes new copies and atomically flips a pointer.
**Q: Half-written WAL record after a crash?** Per-record checksums detect and discard it on replay.

## References

- Mohan et al., *ARIES: A Transaction Recovery Method...* (1992) — the classic WAL algorithm.
- RocksDB WAL documentation.

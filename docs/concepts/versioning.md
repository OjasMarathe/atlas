# Versioning (copy-on-write)

**Area:** DFS · **Phase:** 1 · **Status:** written

## TL;DR

Every edit creates a **new immutable version** rather than overwriting the old one. Atlas stores each
version's metadata (chunk list, owner, timestamps) keyed by version number and advances a "latest"
pointer. Because chunks are content-addressed, **unchanged chunks are shared across versions for
free** — a new version only costs metadata + the chunks that actually changed.

## The problem it solves

Overwriting loses history — you can't recover a prior state, audit changes, or reason about concurrent
edits. Versioning keeps every state, and content-addressing makes it cheap.

## How it works

- Each `RegisterFile` for a `file_id` assigns `version = latest + 1`, stores the `FileMetadata`
  immutably under a per-version key, and advances the `latest` pointer.
- A version's metadata is an ordered **list of chunk ids** (+ owner, whole-file sha256, timestamps).
- **Copy-on-write dedup:** edit one chunk of a 100-chunk file → the new version references the 99
  unchanged chunk ids (same content → same id → already stored) plus 1 new chunk id. Only the changed
  chunk's bytes are actually new.

## Our implementation in Atlas

- `src/metadata/metadata_store.{h,cpp}`, RocksDB-backed. Keys: `f/<file_id>/<zero-padded version>` →
  serialized `FileMetadata`; `L/<file_id>` → the latest version number. `RegisterFile` increments,
  `GetFile(version = 0)` reads latest, `ListVersions` prefix-scans. Timestamps set on register.
- **Verified** in `metadata_store_test`: incrementing + per-file version counters, latest/specific
  lookups, list, persistence across reopen, chunk-list roundtrip.
- The chunk-sharing dedup is a property of content addressing (see [chunking.md](chunking.md)); the
  store just records per-version chunk-id lists.

## Complexity & trade-offs

- Version lookup is `O(log n)` (RocksDB); `ListVersions` is a prefix scan.
- Storage grows with the **number of versions** (metadata) but **not** with unchanged data (chunks are
  shared). [Garbage collection](../concepts/README.md) reclaims chunks no live version references.

## Failure modes & edge cases

- The two writes (version metadata + `latest` pointer) are committed in **one `rocksdb::WriteBatch`
  with `sync=true`**, so they land together or not at all. They must be: this is the system's commit
  point ([ADR-0004](../architecture/adr/0004-replication-consistency.md)), and as separate `Put`s a
  crash between them would durably store a version that `LatestVersion` never returns — the
  just-committed file invisible, with `GetFile(version=0)` serving stale data.
- Concurrent `RegisterFile` on the same `file_id` could race the counter; the **single metadata node**
  ([ADR-0005](../architecture/adr/0005-metadata-single-node-m1.md)) serializes this for M1.

## Alternatives we considered

- **Overwrite in place** — simplest, loses history. Rejected.
- **Full copy per version** — store every version's full bytes; wasteful. Content-addressed chunk
  sharing avoids it.
- **Diff/delta versioning** — compact for tiny edits but complex; chunk-level CoW is a good middle
  ground.

## Interview Q&A

**Q: How do you version without re-storing the whole file?** Content-addressed chunks — a new version
references unchanged chunks by their existing ids and only stores changed chunks.
**Q: What's the `latest` pointer for?** `O(1)` resolution of the current version without scanning.
**Q: Is `RegisterFile` atomic?** Yes — the version blob and the `latest` pointer go in one
`WriteBatch` (sync). Two separate writes would let a crash between them hide a committed file
behind a stale `latest`.
**Q: How is an old version read?** Its metadata lists chunk ids; fetch and reassemble them.

## References

- S3 object versioning / GFS — the pattern.
- [chunking.md](chunking.md) + [sha256-checksums.md](sha256-checksums.md) — the content-addressing that makes CoW cheap.

# Chunking (fixed-size, content-addressed)

**Area:** DFS · **Phase:** 1 · **Status:** written

## TL;DR

Atlas stores a file not as one blob but as a sequence of fixed-size **4 MiB chunks**, each named by
the SHA-256 of its bytes. This is what lets files be spread across nodes, replicated per-chunk,
deduplicated, and verified.

## The problem it solves

Storing whole files as opaque blobs breaks three things a distributed store needs:
- **Distribution** — a 10 GB file can't sit on a node with 8 GB free; chunks scatter across nodes.
- **Replication & repair** — replicating/re-fetching a whole file is coarse; chunks let us replicate
  and heal at 4 MiB granularity.
- **Deduplication** — two files sharing content (or two versions of one file) would be stored twice;
  content-addressed chunks are stored once.

## How it works

1. Split the bytes into 4 MiB pieces (the last is usually smaller).
2. Each chunk's id = `SHA-256(chunk bytes)` → **content addressing**.
3. The metadata service records the file's **ordered list** of chunk ids + where each is stored.

Content addressing buys us, for free:
- **Dedup** — identical bytes → identical id → one stored copy.
- **Integrity** — recompute the hash on read; mismatch = corruption.
- **Immutability** — a chunk never changes; an *edit* produces new chunks + a new file version
  (copy-on-write). This is the keystone of our consistency model
  ([ADR-0004](../architecture/adr/0004-replication-consistency.md)).

## Why fixed-size (and why 4 MiB)

- **Fixed vs content-defined chunking (CDC):** fixed-size is simple and predictable. Its weakness is
  the "insert one byte at the front shifts every boundary" problem, which hurts dedup across edits.
  **CDC** (e.g. Rabin fingerprinting) picks boundaries *by content* so edits stay local — better
  dedup, more complexity. We use fixed-size for M1 (GFS-style); CDC is a future upgrade.
- **4 MiB:** a balance. Too small → the chunk list + metadata explode; too large (GFS used 64 MiB) →
  poor distribution granularity and wasteful re-replication. 4 MiB is a reasonable middle for our
  scale.

## Our implementation in Atlas

- `src/dfs/chunking.{h,cpp}` — `ChunkBytes(bytes, chunk_size = 4 MiB)` → `vector<Chunk{id, data}>`,
  and `Reassemble(chunks)` → original bytes. Ids come from `common/sha256`.
- **Verified** in `tests/chunking_test.cpp`: exact-multiple + remainder splitting, roundtrip,
  `id == SHA-256(data)`, identical-content dedup, empty input, and the 4 MiB boundary.

## Complexity & trade-offs

`O(n)` in file size. The current impl chunks an in-memory `std::string`; for genuinely huge files
we'd stream from disk chunk-by-chunk (a straightforward extension — noted, not needed for M1).

## Failure modes & edge cases

- **Empty file** → zero chunks (roundtrips to empty).
- **Last chunk** smaller than 4 MiB — handled by clamping the final slice.
- **Very large files** — in-memory chunking uses O(file) RAM; stream in production.

## Alternatives we considered

- **Whole-file blobs** — rejected (no distribution/dedup/granular repair).
- **Content-defined chunking (Rabin/LBFS)** — better dedup across edits, more complex; future.
- **Variable-size / by-record chunking** — domain-specific; unnecessary here.

## Interview Q&A

**Q: Why chunk at all?** To distribute a file across machines, replicate/repair at fine
granularity, and dedup identical content.
**Q: Fixed vs content-defined chunking?** Fixed is simple but a front-insert reshuffles every
boundary (bad dedup); CDC picks boundaries by content so edits stay local.
**Q: How does chunking give dedup?** Content addressing — identical bytes hash to the same id, so
they're stored once.
**Q: What happens on a file edit?** New/changed chunks get new ids; unchanged chunks keep theirs; a
new file *version* references the new chunk list (copy-on-write).

## References

- Ghemawat et al., *The Google File System* (SOSP 2003).
- Muthitacharoen et al., *A Low-Bandwidth Network File System* (LBFS, SOSP 2001) — content-defined chunking.

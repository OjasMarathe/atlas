# ADR-0007 — Inverted index on-disk format & posting-list compression

**Status:** Accepted (2026-07-31) — reviewed jointly in PR #4

## Context

Phase 3 needs a persisted inverted index: term → posting list, where each posting carries at
minimum a document ID and term frequency (for BM25, see [concepts/bm25.md](../../concepts/bm25.md)),
and — if we want Phase 3b's phrase search to work without a painful index migration later —
per-occurrence term positions within the document. Per [ADR-0006](0006-search-index-document-partitioned.md),
each search shard's index is **local** (partition by document); per [ADR-0002](0002-storage-engine-rocksdb.md),
all persistent state lives in RocksDB. The roadmap's Phase 3a DoD explicitly requires
"compressed posting lists" — for common terms in a real corpus, posting lists dominate index
size, and naive storage (e.g. one fixed-width integer per posting) wastes space and I/O
disproportionately to how often those terms are actually queried.

## Options considered

### Posting-list encoding

- **Uncompressed fixed-width integers** — trivial to implement/decode; no compression; useful
  only as a correctness baseline to benchmark against.
- **Delta + varint gap encoding (Lucene-style)** — sort doc IDs ascending per posting list,
  store the *gap* between consecutive IDs instead of the ID itself, then varint-encode each
  gap (small gaps → 1 byte, larger → more bytes). Common terms have small average gaps in an
  ID-sorted list, so this typically compresses 4–10× vs. raw integers. Moderate implementation
  complexity; well-documented; the base technique Lucene/Solr use.
- **Roaring bitmaps** — represent each posting list as a compressed bitmap over doc IDs;
  extremely fast set operations (AND/OR/NOT) — exactly what Phase 3b's boolean queries need.
  Downside: a bitmap encodes *presence* only, not term frequency or position, so BM25/phrase
  search would need a parallel structure — doubling the moving parts for M1's scope. Also a
  heavier dependency to pull in now.
- **Frame-of-Reference / PForDelta (block-based bit-packing)** — best compression and
  SIMD-friendly decode speed; the most complex to implement correctly; overkill for a
  demo-scale corpus.

### On-disk layout (RocksDB, per ADR-0002)

- A **term dictionary** column family: `term string → {term_id, document_frequency n(t)}`.
  `n(t)` is exactly what bm25.md's IDF precomputation already needs.
- A **postings** column family: `term_id → encoded posting list` (delta+varint-encoded
  `(docId_gap, term_frequency, position_list)` tuples).
- Using an integer `term_id` instead of the raw term string as the postings key avoids
  repeating variable-length strings across a huge key space, and keeps the hot path
  (posting-list lookup during a query) on fixed-width keys.

## Decision

**Delta + varint gap encoding**, stored in RocksDB across two column families (term
dictionary, postings) as above. Each posting stores `(docId_gap, term_frequency,
position_list)` — we pay the cost of storing positions now, so Phase 3b's phrase search
doesn't require re-encoding the whole index later. Boolean AND/OR/NOT (Phase 3b) will be
implemented as posting-list intersection/union over this same encoding for M1; **roaring
bitmaps are the natural upgrade** if/when boolean queries on large posting lists become a
measured bottleneck — not needed at demo scale.

## Consequences

- **Positive:** matches the roadmap's explicit "compressed posting lists" requirement;
  well-understood, documented technique (straightforward to explain in a concept note);
  storing positions up front avoids a Phase 3b index migration; the term dictionary's `n(t)`
  is reused directly by the BM25 ranker.
- **Negative:** delta encoding requires posting lists to stay **sorted by doc ID**, which
  constrains how incremental indexing (Phase 3b) inserts new postings — appending a new,
  possibly out-of-order doc ID means either re-sorting/re-encoding the tail of the list, or
  buffering new postings separately and merging periodically. Not solved here; flagged for the
  **incremental-indexing** concept note.
- **Deferred:** roaring bitmaps as a boolean-query optimization — explicitly punted until
  there's a measured reason, consistent with ADR-0005/0006's pattern of choosing the simplest
  correct thing for M1 and documenting the upgrade path.
- Adds a concept note to write: **posting-list-compression** (delta+varint gap encoding, with
  a worked example).

## Open question carried forward

Where query parsing (boolean operators, phrase quotes, filters) happens — coordinator vs.
search shard — is a related but distinct decision (it's about the *query* path, not the
*index* format) and is being tracked separately; see the discussion in progress.md pending a
possible ADR-0008.

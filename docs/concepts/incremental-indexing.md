# Incremental Indexing (Update, Delete, Compact)

**Area:** Search engine  ·  **Phase:** 3b  ·  **Status:** drafted

## TL;DR

An inverted index is built for appending, not editing: one document touches every term it
contains, so deleting it properly would mean visiting all of those posting lists. Instead we
**tombstone** — mark the document dead, let queries filter it out, and leave its postings in
place — then reclaim the space in a batched **compaction**. Re-indexing a document is a delete
plus an append, which is why the index is *incremental* without ever mutating a posting list in
the middle.

## The problem it solves

Documents change. Without incremental indexing there are only two options, both bad:

- **Rebuild the whole index** on every edit — correct, and hopelessly expensive.
- **Append the new version anyway** — which is what Atlas did through Phase 3a, and it silently
  returns the same file twice, with the stale copy still matching its old content.

The tempting fix is to edit posting lists in place: find every term the old document contained
and splice out its entry. That fails for three reasons:

1. **Cost.** A 500-term document appears in 500 posting lists. Deleting it is 500 lookups plus
   500 vector erases, each shifting memory.
2. **Ordering.** Posting lists are sorted by doc id and delta-encoded
   ([posting-list-compression](posting-list-compression.md)). Removing an entry from the middle
   invalidates the gap that follows it, forcing a re-encode of the tail.
3. **Concurrency.** Readers are walking those exact lists. Mutating them underneath a query
   means locking the whole index, or a much more careful lock-free design.

So the standard answer — Lucene's, and ours — is: *never modify a posting list; make the reader
skip what's dead.*

## How it works

### Tombstones

Deleting sets a flag on the document, not on its postings:

```
delete "a.md"        ->  docs_[0].deleted = true
                         by_file_id_.erase("a.md")
                         live_documents_--, live_length_ -= its length

posting list "chunk" :  [ {doc 0} {doc 1} ]      <- unchanged, doc 0 still physically present
query for "chunk"    :  [ {doc 0} {doc 1} ]  ->  filter IsLive  ->  [ doc 1 ]
```

The cost moves from write time (expensive, blocking) to read time (a cheap flag check per
posting), and the write becomes O(1).

### Update = delete + append

```cpp
DocId InvertedIndex::IndexDocument(std::string file_id, std::string_view text, fields) {
  DeleteDocument(file_id);   // tombstone the previous version, if any
  ...                        // append a brand-new document with a fresh DocId
}
```

The old doc id is never reused, so any posting still referring to it stays unambiguously dead.

### Statistics must follow the living

This is the subtle part. BM25 reads `N`, `avgdl` and `n(t)` from the index
([bm25](bm25.md)), and if tombstones leak into those, every score drifts:

- `DocumentCount()` returns `live_documents_`, maintained on each add/delete.
- `AverageDocumentLength()` divides `live_length_` by `live_documents_`.
- `DocumentFrequency(t)` **counts only live postings**, so IDF doesn't treat a deleted document
  as evidence that a term is common.

### Compaction

Tombstones accumulate, so eventually the space is reclaimed in one pass:

```
1. assign each live document a new dense DocId          (remap old -> new)
2. rewrite every posting list, dropping dead postings and renumbering the survivors
3. drop terms whose every posting was dead
4. rebuild the file_id -> DocId map
```

Compaction invalidates every previously returned `DocId`, which is exactly why the external
identity of a document is its `file_id` string and `DocId` is an internal detail.

## Our implementation in Atlas

- **Where it lives:** `src/search/inverted_index.{h,cpp}` — `IndexDocument`, `DeleteDocument`,
  `Compact`, `IsLive`. `SearchEngine::Compact()` also rebuilds the suggestion structures.
- **Filtering happens in `Evaluate`'s Term case** (`src/search/search_engine.cpp`), so every
  boolean set operation above it already sees live-only ids. Doing it once at the leaves beats
  filtering after each union/intersection.
- `AllDocuments()` returns live ids only, so `NOT` complements against the living.
- **Known gap — vocabulary is not decremented.** The autocomplete/spell-correction vocabulary
  keeps words from deleted documents. That's deliberate: tracking per-document surface forms
  purely to decrement counters costs more than the occasional stale suggestion. `Compact()`
  doesn't fix it either; a rebuild does.
- **No automatic compaction trigger.** Real systems compact when the tombstone ratio crosses a
  threshold; ours is manual, which is honest for M1 but means a long-running shard grows.

## Complexity & trade-offs

| Operation | Cost |
|---|---|
| Index a document | `O(terms)` — unchanged by this feature |
| Delete | `O(1)` — one flag, one map erase |
| Update | `O(terms)` — a delete plus an append |
| Query overhead | one `IsLive` check per posting scanned |
| `Compact` | `O(total postings + documents)`, one pass |
| Wasted space | proportional to tombstoned documents, until compaction |

The trade is deliberate: writes get cheap and non-blocking, reads pay a small constant, and the
real cost is deferred into a batch job that can run when convenient.

## Failure modes & edge cases

- **Deleting an unknown file** returns false rather than throwing.
- **Re-indexing after deleting** works — the file id is simply absent, so the delete is a no-op.
- **`DocId`s are invalidated by `Compact()`**. Any cached id — including one held across an RPC —
  is dangling afterwards. Only `file_id` is stable.
- **Stale suggestions** survive deletion (see above).
- **Terms with no live documents** still occupy the map until compaction, so `UniqueTerms()`
  overcounts in the meantime.
- **A tombstone-heavy index degrades quietly**: queries still scan the dead postings, so a shard
  that has churned heavily gets slower without any error surfacing.
- **No concurrency control here.** `SearchServiceImpl` serializes access with a mutex; the index
  itself assumes a single writer.

## Alternatives we considered

- **In-place deletion from posting lists** — intuitive, and wrong for the three reasons above
  (cost, delta-encoding order, concurrent readers).
- **Full rebuild on change** — trivially correct; unusable at any real write rate.
- **Immutable segments + background merge (Lucene's real design)** — each flush writes a new
  small immutable index; searches query all segments and merge results; a background job merges
  segments and drops deleted documents. Strictly better: it gives crash-safe persistence and
  bounded merge work, and it lets compressed lists stay immutable. Our single mutable index with
  tombstones is the same *idea* (never edit in place, reclaim in batch) at one-segment scale, and
  segments are the natural next step.
- **Reference-counted postings** — per-posting bookkeeping cost with no real benefit here.

## Interview Q&A

**Q: Why not just remove a document's entries from the posting lists?**
Because the document appears in one list per distinct term it contains, those lists are sorted
and delta-encoded so a mid-list removal invalidates the following gaps, and readers are walking
them concurrently. It converts an O(1) write into an expensive, blocking one.

**Q: What is a tombstone, and where does the cost go?**
A flag marking a document dead while its postings stay put. The cost moves from write time to
read time (a liveness check per posting) plus a batched compaction later.

**Q: What breaks if deleted documents leak into index statistics?**
BM25 skews. `N` and `avgdl` shift, and `n(t)` overstates how common a term is, deflating its
IDF. So document frequency must count live postings only.

**Q: How does updating a document differ from adding one?**
It doesn't, structurally: update is delete-then-append with a fresh doc id. The old id is never
reused, so lingering postings stay unambiguously dead.

**Q: When would you compact, and what does it break?**
When the tombstone ratio makes the wasted scanning or memory unacceptable. It renumbers doc ids,
so every cached id is invalidated — external identity has to be a stable key like the file id.

**Q: How does this scale to a real system?**
Immutable segments: flush new documents into a small new index, search across all segments, and
merge them in the background, dropping deleted documents at merge time. That keeps every
compressed list immutable and bounds the work per merge.

## References

- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*, ch. 4 — dynamic
  indexing, logarithmic merging.
- Lucene's `IndexWriter` / segment merge policy and its deleted-docs bitset.
- O'Neil et al., *The Log-Structured Merge-Tree* (1996) — the same "never update in place,
  reclaim by merging" idea underneath RocksDB ([ADR-0002](../architecture/adr/0002-storage-engine-rocksdb.md)).

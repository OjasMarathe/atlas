# Posting-List Compression (Delta + Varint)

**Area:** Search engine  ·  **Phase:** 3b  ·  **Status:** drafted

## TL;DR

Posting lists dominate an inverted index's size, and they're the thing every query reads. Two
cheap transforms shrink them dramatically: store the **gap** between consecutive document ids
instead of the ids themselves (they ascend, so gaps are small), then write each number as a
**varint** — one byte for values under 128 instead of a fixed four. On Atlas's own docs corpus
this takes the postings from 107.5 KiB to a 70.9 KiB whole-index snapshot, a **1.52×** reduction
that also carries the document table and vocabulary.

## The problem it solves

A posting list for a common term holds one entry per matching document. Store a doc id as a
fixed 32-bit integer and every entry costs 4 bytes whether the id is 3 or 3,000,000. For a term
appearing in a million documents that's 4 MB for the ids alone — and index size is not a
vanity metric: posting lists are read from disk on the query path, so **bytes are latency**.
Halving the bytes roughly halves the I/O.

The naive fix — a general-purpose compressor like gzip over the list — works but destroys the
property we depend on: you can no longer decode from the middle, so skip pointers and
early-termination stop working. We want a format that is *smaller* while staying *sequentially
decodable from a known offset*.

## How it works

### Step 1 — delta (gap) encoding

Posting lists are sorted by ascending doc id ([inverted-index](inverted-index.md) keeps that
invariant precisely so this works). So instead of the ids, store the difference from the
previous one:

```
doc ids :  1000000  1000001  1000004  1000009
gaps    :  1000000        1        3        5
```

The ids need 4 bytes each; every gap after the first fits in one. The denser the list, the
smaller the gaps — and the terms with the longest posting lists are exactly the dense ones, so
the compression is best where it matters most.

### Step 2 — varint encoding

A varint spends 7 bits of each byte on the value and the top bit as "another byte follows":

```
value 5     -> 0000_0101                     (1 byte)
value 300   -> 1010_1100  0000_0010          (2 bytes)
              ^ more                ^ done
              low 7 bits = 0101100  high bits = 10
```

Values 0–127 cost one byte, up to 16,383 cost two, and so on. Combined with delta encoding —
where almost every number is tiny — the common case is one byte per doc id.

### Worked example

Three postings for a term, with positions:

```
{doc 3, tf 2, pos [5, 40]}   {doc 9, tf 1, pos [0]}   {doc 1000, tf 3, pos [1, 2, 900]}

fixed width: 3 postings x (doc + tf + count) x 4B  + 6 positions x 4B  =  60 bytes
delta+varint:
  count=3                                          -> 1B
  gap 3,  tf 2, npos 2, posgaps 5, 35              -> 5B
  gap 6,  tf 1, npos 1, posgaps 0                  -> 4B
  gap 991,tf 3, npos 3, posgaps 1, 1, 898          -> 2+1+1+1+1+2 = 8B
                                                      ------------
                                                            18 bytes
```

Note positions get the same treatment: they're ascending within a posting, so they're stored as
gaps too.

## Our implementation in Atlas

- **Where it lives:** `src/search/posting_codec.{h,cpp}` — `PutVarint`/`GetVarint` and
  `EncodePostingList`/`DecodePostingList`. `InvertedIndex::Serialize()`/`Load()` build the
  whole-index snapshot on top.
- **Decisions from [ADR-0007](../architecture/adr/0007-inverted-index-format-compression.md):**
  delta+varint over roaring bitmaps (a bitmap stores presence only, so BM25's term frequencies
  and phrase positions would need a parallel structure) and over PForDelta (better ratio,
  materially harder to get right, unnecessary at demo scale).
- **Positions are stored**, which costs real space — they're the largest single contributor —
  but retrofitting them into an already-encoded index would mean rewriting all of it, so
  ADR-0007 pays now to avoid a migration.
- `Load()` builds into a temporary and only commits on success, so a truncated or corrupt buffer
  leaves the existing index untouched rather than half-populated.

```cpp
std::string EncodePostingList(const PostingList& postings) {
  std::string out;
  PutVarint(postings.size(), &out);
  DocId previous_doc = 0;
  for (const Posting& posting : postings) {
    PutVarint(posting.doc_id - previous_doc, &out);   // the gap, not the id
    previous_doc = posting.doc_id;
    PutVarint(posting.term_frequency, &out);
    PutVarint(posting.positions.size(), &out);
    std::uint32_t previous_position = 0;
    for (const std::uint32_t position : posting.positions) {
      PutVarint(position - previous_position, &out); // positions ascend too
      previous_position = position;
    }
  }
  return out;
}
```

## Complexity & trade-offs

- **Encode / decode:** `O(n)` in the number of postings, one pass, no allocation beyond the
  output buffer.
- **Measured on `docs/`** (19 documents, 1,884 terms): postings at fixed width 107.5 KiB → whole
  serialized index 70.9 KiB (**1.52×**), produced in ~1 ms. The ratio is modest here because a
  small corpus has small doc ids that were already cheap; the gain grows with corpus size, since
  fixed-width cost stays at 4 bytes per id while gaps stay near 1.
- **What we gave up:** random access. A varint's length is only known by reading it, so you
  cannot jump to posting *k* — you must decode from a known start. This is exactly why
  [skip pointers](boolean-phrase-search.md) are stored as explicit checkpoints rather than
  computed by arithmetic, and why real engines encode in fixed-size *blocks* so each block is an
  independently decodable entry point.
- **Mutation becomes expensive.** Appending an out-of-order doc id means re-encoding the tail,
  which is what pushes production systems toward immutable segments —
  see [incremental-indexing](incremental-indexing.md).

## Failure modes & edge cases

- **Truncated input** — `GetVarint` returns false rather than reading past the end; every
  decoder call site checks it, and `Load()` discards a partial parse.
- **Overlong encodings** — a varint claiming more than 64 bits of payload is rejected instead of
  silently wrapping.
- **Unsorted input** would produce a negative gap that wraps to an enormous unsigned value,
  silently corrupting the list. The index's append-in-doc-order invariant is what prevents this;
  it is an invariant the codec *relies on* and cannot itself check cheaply.
- **Empty posting list** round-trips as a single zero byte.
- **A tombstoned document's postings still get encoded** — compaction, not compression, is what
  reclaims them.

## Alternatives we considered

- **Fixed 32-bit integers** — trivial and randomly addressable, but 4 bytes for the number 1.
  Kept as the baseline the tests measure against.
- **Roaring bitmaps** — excellent for boolean set operations and very compact for dense sets, but
  presence-only: no term frequencies, no positions. ADR-0007 records it as the upgrade if boolean
  queries ever dominate.
- **PForDelta / Frame-of-Reference** — bit-pack a block to the width of its largest value and
  store outliers separately; better ratio and SIMD-friendly, at a real complexity cost.
- **Elias-Fano** — near-optimal for monotone sequences and supports fast random access; more
  intricate, and it doesn't carry the payloads (tf, positions) we need alongside.
- **Generic compression (gzip/zstd) over the whole list** — good ratios, but it destroys
  positional decoding and forces whole-list decompression per query.

## Interview Q&A

**Q: Why does delta encoding help?**
Posting lists are sorted, so consecutive ids differ by a small amount. Storing that difference
turns large numbers into small ones, and small numbers are what varint encodes cheaply.

**Q: How does a varint work, and when is it a bad idea?**
Seven payload bits per byte plus a continuation bit. It's a loss when values are routinely large
— a uniformly random 32-bit number takes five bytes as a varint versus four fixed — which is
precisely why you delta-encode first.

**Q: What do you give up by compressing posting lists?**
Random access. You can't index into the list, so you need explicit skip checkpoints or
fixed-size blocks to jump; and mutation gets expensive because changing one entry can require
re-encoding everything after it.

**Q: Why store positions if phrase search is a later phase?**
Because adding them afterwards means rewriting the entire encoded index. Storage is cheap
relative to a migration.

**Q: How would you decode only part of a huge posting list?**
Encode in fixed-size blocks (say 128 postings) with a skip table holding each block's first doc
id and byte offset. That's what Lucene does, and it restores the random access plain varints
give up.

## References

- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*, ch. 5 — index
  compression, gap encoding, variable-byte codes.
- Lemire & Boytsov, *Decoding billions of integers per second through vectorization* (2015).
- Zobel & Moffat, *Inverted Files for Text Search Engines* (2006).
- Lucene's `PostingsFormat` / `ForUtil` — production block-based encoding.

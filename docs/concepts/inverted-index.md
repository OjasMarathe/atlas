# The Inverted Index

**Area:** Search engine  ·  **Phase:** 3  ·  **Status:** drafted

## TL;DR

An inverted index maps **term → the list of documents containing it**, instead of document →
its words. That inversion is what makes search sublinear: answering "which documents mention
`replic`?" becomes one hash lookup returning a precomputed list, rather than a scan of every
document. Each entry (a *posting*) carries the document id, the term frequency BM25 needs, and
the positions phrase search will need.

## The problem it solves

The naive search is a linear scan: for each document, check whether it contains the query term.
That's `O(N · document_length)` per query — on a corpus of 10,000 documents averaging 500 terms,
5 million comparisons to answer one keyword query, and it gets worse with every document added.
Worse, it does all that work *again* for the next query, having learned nothing.

The fix is to pay the cost once, at index time, and reorganize the data by the thing queries
actually ask about — the term:

```
Documents (forward)                    Inverted index
  hashing.md  -> [consist, hash, ring]   consist -> [hashing.md]
  wal.md      -> [write, ahead, log]     hash    -> [hashing.md]
                                          ring    -> [hashing.md]
                                          write   -> [wal.md]
                                          ...
```

A query now touches only the posting lists of its own terms. Query cost becomes proportional to
how many documents actually match, not to the size of the corpus.

## How it works

### Structure

```
term ──► PostingList = [ Posting, Posting, ... ]   (sorted by ascending doc_id)

Posting {
  doc_id           // which document
  term_frequency   // f(t,D) — how many times, for BM25
  positions[]      // where, for phrase search (Phase 3b)
}
```

Indexing a document runs it through the [analysis pipeline](tokenization-stemming.md), then for
each resulting term appends to that term's posting list.

### The sorted-by-doc_id invariant

Posting lists are kept in ascending doc-id order. This is not incidental — three things depend
on it:

1. **Boolean queries** intersect and union posting lists as a linear merge, which requires both
   sides sorted (see [boolean-phrase-search](boolean-phrase-search.md)).
2. **Compression**: [ADR-0007](../architecture/adr/0007-inverted-index-format-compression.md)
   stores *gaps* between consecutive doc ids rather than the ids themselves. Gaps are only small
   (and therefore only compressible) if the list ascends.
3. **Skip pointers** can only skip forward through a sorted list.

Atlas gets the invariant for free: documents are assigned sequential doc ids as they arrive, so
appending to a term's list preserves order without ever sorting.

### Worked example

Indexing `"the chunk and chunk"` (positions come from the pre-filter token stream, so the
dropped stop words leave gaps):

```
tokens:    the(0)  chunk(1)  and(2)  chunk(3)
analyzed:           chunk@1           chunk@3     ("the", "and" are stop words)

index:  chunk -> [ {doc_id: 0, tf: 2, positions: [1, 3]} ]
```

Then indexing `"chunk replication"` as doc 1:

```
        chunk  -> [ {0, tf:2, [1,3]}, {1, tf:1, [0]} ]   ← appended, still ascending
        replic -> [ {1, tf:1, [1]} ]
```

`DocumentFrequency("chunk")` is now 2 — just the posting list's length, which is exactly the
`n(t)` that [BM25's](bm25.md) IDF needs. The index stores the statistic by construction.

## Our implementation in Atlas

- **Where it lives:** `src/search/inverted_index.{h,cpp}`. `AddDocument()` indexes; `Lookup()`
  returns a term's posting list; `DocumentFrequency()`, `DocumentLength()`, and
  `AverageDocumentLength()` feed the ranker.
- **In-memory for M1**: `unordered_map<string, PostingList>`. Persisting to RocksDB with
  delta+varint compressed posting lists is ADR-0007's decision, deferred to Phase 3b — the
  interface above is what that change has to preserve.
- **DocId is a dense `uint32_t`**, assigned sequentially, while the externally visible identity
  stays the `file_id` string. Small integers keep posting lists compact and give delta encoding
  ascending values to take gaps between; the mapping back to `file_id` happens once, at the end
  of a query, for the handful of documents that made the top-K.
- **Transparent hashing** (`TermHash` with `is_transparent`) so a `string_view` lookup doesn't
  allocate a `std::string` on every query-time probe.
- **The index stores stems, never surface forms.** `Lookup("replicating")` returns nothing;
  `Lookup("replic")` finds it. That is the analyzer symmetry from
  [tokenization-stemming](tokenization-stemming.md), viewed from the index side.

```cpp
DocId InvertedIndex::AddDocument(std::string file_id, std::string_view text) {
  const std::vector<Term> terms = Analyze(text);
  const auto doc_id = static_cast<DocId>(docs_.size());
  docs_.push_back({std::move(file_id), terms.size()});
  total_length_ += terms.size();

  for (const Term& term : terms) {
    PostingList& list = postings_[term.text];
    if (list.empty() || list.back().doc_id != doc_id) list.push_back({doc_id, 0, {}});
    ++list.back().term_frequency;
    list.back().positions.push_back(term.position);
  }
  return doc_id;
}
```

Because the same document's terms all arrive together, `list.back()` is the only posting that
can belong to the current document — so tf accumulation is O(1) with no search.

## Complexity & trade-offs

| Operation | Cost |
|---|---|
| Index a document | `O(terms)` — one hash probe and an append per term |
| Look up a term | `O(1)` average |
| `DocumentFrequency` | `O(1)` — the posting list's length |
| Query for term t | `O(\|postings(t)\|)` — proportional to matches, not corpus size |
| Space | `O(total term occurrences)`, dominated by positions |

- **We bought** query time that scales with the number of *matches* instead of the corpus.
- **We gave up** write simplicity and space: every document costs index maintenance, and the
  index is roughly the size of the corpus's term occurrences. Storing positions (for Phase 3b
  phrase search) is the single largest contributor — a deliberate bet from ADR-0007 that paying
  now beats re-indexing everything later.

## Failure modes & edge cases

- **No document deletion or update.** `AddDocument` always appends a new document; re-indexing
  the same `file_id` creates a *second* copy that both match. Real systems handle this with a
  deleted-docs bitmap plus periodic segment merges. This is the main gap before Phase 3b's
  incremental indexing.
- **Delta encoding constrains insertion.** Once posting lists are compressed as gaps
  (ADR-0007), appending an out-of-order doc id means re-encoding the list's tail. Production
  systems avoid this with immutable segments merged in the background rather than one mutable
  list.
- **Stop-word-only documents** are indexed (they count toward `N` and shift `avgdl`) but
  contribute no terms and can never be retrieved. Correct, but worth knowing when corpus
  statistics look off.
- **Hot terms.** A term present in nearly every document has a posting list as long as the
  corpus, so a query containing it degenerates to a full scan — while its IDF makes it
  contribute almost nothing to the score. Stop-word removal handles the worst offenders;
  WAND/block-max pruning is the real answer.
- **Memory is unbounded.** The whole index lives in RAM for M1; a large corpus simply runs out.
  That's what ADR-0007's RocksDB persistence is for.
- **Out-of-range DocIds** throw `std::out_of_range` rather than reading past the end.

## Alternatives we considered

- **Forward index only (linear scan)** — trivially correct, no index to maintain, and
  unusable beyond a toy corpus.
- **Trigram / n-gram index** — supports substring and fuzzy matching without an analyzer, but
  much larger and it loses term boundaries. Useful for code search; wrong default for prose.
- **Suffix array / FM-index** — excellent for substring search over one large text; a poor fit
  for ranked multi-term retrieval over many documents.
- **Roaring bitmap per term** — very fast boolean set operations, but a bitmap stores only
  *presence*, so BM25's term frequencies and phrase positions would need a parallel structure.
  ADR-0007 records this as the upgrade path if boolean queries ever become the bottleneck.
- **Term-partitioned distribution** — rejected in [ADR-0006](../architecture/adr/0006-search-index-document-partitioned.md);
  we partition by document instead, so each shard owns a complete index over its own documents.

## Interview Q&A

**Q: Why is it called "inverted"?**
The natural (forward) layout maps document → its terms. The index inverts that to term →
documents, which is the direction queries actually traverse.

**Q: Why must posting lists be sorted by document id?**
Boolean queries merge lists linearly (which needs sorted inputs), delta compression needs
ascending values to produce small gaps, and skip pointers can only jump forward. All three break
on an unsorted list.

**Q: What exactly is in a posting, and why?**
Document id (which document), term frequency (BM25's `f(t,D)`), and positions (phrase search).
Positions are stored even though phrase search is a later phase, because retrofitting them means
re-encoding the entire index.

**Q: Where does BM25's `n(t)` come from?**
It's the length of the term's posting list — the index maintains the statistic for free.

**Q: What's the hardest part of *updating* an inverted index?**
Deletion and modification. Posting lists are append-optimized and (once compressed) order-
dependent, so real systems don't mutate them — they write immutable segments, mark deletions in
a side bitmap, and merge segments in the background.

**Q: How does this scale past one machine?**
Partition by document: each shard indexes its own documents and answers over them; a coordinator
scatters the query and merges local top-Ks. The cost is that corpus-global statistics (IDF,
avgdl) become per-shard approximations — see [bm25.md](bm25.md).

## References

- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*, ch. 1–2 — the standard
  treatment of the inverted index and posting-list merging.
- Zobel & Moffat, *Inverted Files for Text Search Engines* (ACM Computing Surveys, 2006).
- Lucene's `PostingsFormat` documentation — how a production index lays out doc ids, frequencies
  and positions in separate streams.

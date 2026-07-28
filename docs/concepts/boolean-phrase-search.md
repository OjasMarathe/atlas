# Boolean Search (AND / OR / NOT), Skip Pointers, and Phrase Matching

**Area:** Search engine  ·  **Phase:** 3  ·  **Status:** drafted — boolean implemented, phrase matching designed but not yet built (3b)

## TL;DR

Boolean retrieval answers *which* documents qualify; [BM25](bm25.md) then answers *in what
order*. Because posting lists are sorted by document id, `AND` is a linear intersection, `OR` a
union, and `NOT` a difference — all merges, no sorting. **Skip pointers** make intersection
skip over runs of non-matching ids instead of walking them. Phrase search is the same idea one
level down: intersect the *positions* inside the documents that already matched.

## The problem it solves

Ranking alone isn't enough. Three things need a set, not a score:

- **Precision.** `chunk AND replication` should exclude documents that merely mention one term.
- **Exclusion.** There is no BM25 score meaning "must not contain this word" — `NOT` is a set
  operation or it doesn't exist.
- **Efficiency.** Scoring is the expensive part. Narrowing to a candidate set first means BM25
  only touches documents that can actually qualify.

The naive implementation is to materialize each term's document set into a hash set and use set
operations. That works but allocates a hash set per term per query, and throws away the one
property posting lists already have: **they're sorted**. A merge exploits that ordering directly,
in one pass, with no allocation and no hashing.

## How it works

### The three operations as merges

Two sorted lists, two cursors; compare the ids under the cursors and advance:

```
AND (intersection)          OR (union)                 NOT (difference)
a: [1, 3, 5, 7, 9]          a: [1, 3, 5]               a: [1, 2, 3, 4]
b: [3, 4, 5, 9, 12]         b: [3, 4, 9]               b: [2, 4]
   ↓                           ↓                          ↓
   [3, 5, 9]                   [1, 3, 4, 5, 9]            [1, 3]
```

Each is `O(|a| + |b|)` and the output is itself sorted — so operations compose into a tree
without ever re-sorting.

`NOT` needs a universe to complement against: `NOT x` is *every document in the shard* minus the
documents containing `x`.

### Skip pointers — the optimization

Intersection stalls when one list contains a long run of ids the other doesn't. Walking that run
is pure waste: every id in it is known to be too small to match.

```
a: [1, 2, 3, ..., 998, 999, 1000]    ← cursor crawls through 997 ids
b: [1, 1000]                            that can never match
```

**Skip pointers** place a checkpoint every `√n` postings, so the merge can leap:

```
a: [1 ....... 250 ....... 500 ....... 750 ....... 1000]
    ^checkpoint  ^checkpoint  ^checkpoint  ^checkpoint
                          target = 1000
    jump, jump, jump, then scan the last short stretch
```

`√n` is the classic stride: with `n/√n = √n` checkpoints each covering `√n` postings, it
balances the cost of storing and consulting checkpoints against the distance each one saves.

Crucially, **skip pointers change the cost, never the answer** — which is exactly how they
should be tested.

### Phrase matching (designed, not yet implemented — Phase 3b)

`"consistent hashing"` as a phrase means the two terms are *adjacent*, not merely both present.
The algorithm reuses everything above, one level down:

1. Intersect the posting lists of `consist` and `hash` → documents containing both.
2. For each surviving document, merge the two **position** lists looking for
   `position(hash) == position(consist) + 1`.

This is why [the analysis pipeline](tokenization-stemming.md) records positions from the
*pre-filter* token stream: a dropped stop word must still occupy its slot, or `"chunk ring"`
would falsely match "chunk **of the** ring".

## Our implementation in Atlas

- **Set operations + skip pointers:** `src/search/postings.{h,cpp}` — `Intersect`, `Union`,
  `Difference`, and `SkipList`.
- **Query parsing:** `src/common/query/parser.{h,cpp}` — deliberately *not* inside the search
  shard. Phase 4's coordinator parses a query once and ships the tree to every shard, so parsing
  cannot live in shard-only code. Precedence is `NOT` > `AND` > `OR`, with parentheses.
- **Evaluation:** `SearchEngine::Evaluate` in `src/search/search_engine.cpp` walks the parsed
  tree and returns the sorted candidate list, which the ranker then scores.

**Design decision — operators are UPPERCASE keywords.** `AND`/`OR`/`NOT` are recognized during
lexing, *before* analysis. This is forced by a subtlety: `not` is itself a stop word, so if
operators were recognized after analysis they would already have been deleted. Lowercase `and`
in a query is an ordinary word (and gets dropped as a stop word).

**Design decision — the implicit operator is OR.** Adjacent terms with no keyword
(`chunk replication`) combine with `OR`, not `AND`. With BM25 doing the ranking, widening recall
and letting the score sort it out beats returning nothing because one term was missing — a
document matching both naturally outranks one matching only one. `Options::implicit_operator`
makes this configurable.

**Design decision — negated terms don't contribute to the score.** `PositiveTerms()` skips
subtrees under `NOT`. Scoring a document on a term it was selected for *not* containing would be
incoherent.

```cpp
std::vector<DocId> Intersect(const std::vector<DocId>& a, const std::vector<DocId>& b) {
  const SkipList skip_a(a), skip_b(b);
  std::size_t i = 0, j = 0;
  std::vector<DocId> out;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j])      { out.push_back(a[i]); ++i; ++j; }
    else if (a[i] < b[j])  { i = skip_a.Advance(a, i, b[j]); }  // leap, don't crawl
    else                   { j = skip_b.Advance(b, j, a[i]); }
  }
  return out;
}
```

## Complexity & trade-offs

- **Union / difference:** `O(|a| + |b|)`, always a full merge.
- **Intersection:** `O(|a| + |b|)` worst case; with skip pointers, closer to
  `O(min(|a|,|b|) · √(max))` when list sizes are lopsided — the common case, since query terms
  differ wildly in frequency.
- **Building a `SkipList`:** `O(n/√n) = O(√n)` checkpoints, built per query. For a persisted
  index the checkpoints would be stored alongside the postings rather than recomputed.
- **What we gave up:** an `AND` of two huge lists is still a full merge, and a bare `NOT`
  materializes the entire document universe before subtracting. Real engines reorder clauses
  (smallest list first) and push `NOT` into the intersection instead of complementing eagerly.

## Failure modes & edge cases

- **`NOT` under an implicit OR is a footgun.** `chunk NOT search` parses as
  `chunk OR (NOT search)` — the union with "everything lacking *search*" swallows the exclusion.
  The user almost always means `chunk AND NOT search`. Our demo tool calls this out explicitly.
- **A bare `NOT` query** returns every document that lacks the term, all with score 0 — there is
  no positive term to rank on. Some engines reject a purely negative query outright.
- **Stop words in boolean clauses vanish.** `chunk AND the` degrades to `chunk`, because the
  clause analyzed away. We drop the empty clause rather than fail, and a clause left with a
  single survivor is unwrapped rather than kept as a one-child operator node.
- **Empty and malformed queries are different things.** An all-stop-word query is *valid* and
  returns nothing; `(chunk AND` is a parse error and reports one.
- **Duplicate ids would break everything.** Every operation assumes sorted, duplicate-free
  lists. The index guarantees this by appending one posting per (term, document).
- **Phrase search is not implemented.** Quoted phrases are not yet parsed; positions are stored
  and ready for it in Phase 3b.

## Alternatives we considered

- **Hash sets instead of merges** — simpler to write, but discards the sortedness we already
  have, allocates per query, and produces unsorted output that then needs re-sorting to compose.
- **Roaring bitmaps** — genuinely faster for large boolean workloads and the natural upgrade
  ([ADR-0007](../architecture/adr/0007-inverted-index-format-compression.md) records it), but a
  bitmap holds only presence, so BM25 frequencies and phrase positions need a parallel structure.
- **Galloping (exponential) search instead of skip pointers** — comparable benefit for in-memory
  arrays and needs no extra structure, but it depends on random access, which a compressed
  on-disk posting list doesn't have. Skip pointers survive the move to disk; galloping doesn't.
- **Skip *lists* (probabilistic, multi-level)** — more general, more pointer-chasing, and
  unnecessary for a flat sorted array.
- **WAND / block-max ranked retrieval** — skips documents that cannot make the top-K at all, a
  strictly stronger optimization. It belongs with a persisted index and per-block score bounds;
  well past M1.

## Interview Q&A

**Q: Why are boolean operations merges rather than set intersections?**
Posting lists are already sorted by document id, so two cursors walking in lockstep give the
answer in one linear pass, no hashing, no allocation, and the output stays sorted so operations
compose into a tree.

**Q: What problem do skip pointers solve, and why `√n`?**
They stop the merge from crawling through long runs of ids that cannot match. With a stride of
`√n` you store `√n` checkpoints and each can skip up to `√n` postings — the point where the cost
of checkpoints and the distance saved balance out.

**Q: How would you test a skip-pointer implementation?**
Against a plain linear merge on the same input. Skipping is an optimization, so any divergence
in output is a bug by definition — property-check equality on lopsided lists, which is exactly
where skipping engages.

**Q: How does phrase search differ from `AND`?**
`AND` needs both terms present anywhere; a phrase needs them adjacent. You first intersect the
posting lists, then intersect the *position* lists inside each surviving document looking for
consecutive offsets.

**Q: Why must query operators be parsed before text analysis?**
Because analysis destroys them — `not` is a stop word and would simply be deleted. Operators are
therefore matched as uppercase keywords during lexing, and only the remaining words are analyzed.

**Q: Why is `NOT` expensive, and how would you make it cheap?**
Complementing against the whole shard materializes every document id. Instead, push it into the
enclosing `AND`: iterate the positive side and skip ids present in the negated list, so you never
build the universe at all.

## References

- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*, ch. 1–2 — boolean
  retrieval, posting-list merging, skip pointers, and positional indexes.
- Broder et al., *Efficient Query Evaluation using a Two-Level Retrieval Process* (2003) — WAND.
- Lemire & Boytsov, *Decoding billions of integers per second* (2015) — why layout and skipping
  dominate posting-list performance in practice.

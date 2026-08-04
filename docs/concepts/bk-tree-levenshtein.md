# BK-Tree & Levenshtein Distance (Spell Correction)

**Area:** Search engine  ·  **Phase:** 3b  ·  **Status:** drafted

## TL;DR

**Levenshtein distance** counts the fewest single-character edits turning one word into another,
which is how you measure "close to what they typed". A **BK-tree** makes searching by that
measure cheap: because edit distance obeys the triangle inequality, comparing the query to one
node lets you *prove* whole branches contain nothing within `k` edits, and skip them without
looking. Atlas uses it for "did you mean?" — `hashign` → `hashing`, `replicaton` → `replication`.

## The problem it solves

A typo produces zero results even though the corpus obviously contains the intended word.
Fixing it means finding vocabulary entries within a couple of edits of what was typed.

The brute-force answer — compute the distance to every word and keep the close ones — is
correct but costs a full dynamic-programming pass per word. On a 100k-word vocabulary that's
100k matrix computations for one lookup, on the interactive path.

Ordinary indexes are no help: a hash map needs exact keys, and a
[trie](trie-autocomplete.md) needs a correct *prefix* — but `hashign` doesn't share a useful
prefix with `hashing`, and a typo in the first letter defeats it entirely. Nearness here isn't
lexicographic, so we need an index over the *metric* itself.

## How it works

### Levenshtein distance

The minimum number of insertions, deletions, or substitutions converting `a` into `b`. Computed
with dynamic programming, where `d[i][j]` is the distance between the first `i` characters of
`a` and the first `j` of `b`:

```
d[i][j] = min( d[i-1][j]   + 1          // delete
             , d[i][j-1]   + 1          // insert
             , d[i-1][j-1] + (a[i]!=b[j]) )  // substitute (free if the characters match)

        s  i  t  t  i  n  g
     0  1  2  3  4  5  6  7
  k  1  1  2  3  4  5  6  7
  i  2  2  1  2  3  4  5  6
  t  3  3  2  1  2  3  4  5
  t  4  4  3  2  1  2  3  4
  e  5  5  4  3  2  2  3  4
  n  6  6  5  4  3  3  2  3          kitten -> sitting = 3
```

Each cell depends only on the row above and the cell to its left, so two rows suffice — that's
the `O(min(n,m))` memory version we implement.

### Why edit distance is a *metric*

Three properties hold: `d(a,b) = 0` iff `a = b`; `d(a,b) = d(b,a)`; and the triangle inequality
`d(a,c) ≤ d(a,b) + d(b,c)`. The third is what a BK-tree is built on.

### The BK-tree

Pick any word as the root. Insert each new word by computing its distance to the current node and
descending the child edge labelled with that distance, creating it if absent. **Edge labels are
distances to the parent.**

```
            chunk
          /   |   \
        1     2     5
       /      |      \
   chunks   check   storage
                        \
                         4
                          \
                        replica
```

To find everything within `k` of a query, compute `d = distance(query, node)`. By the triangle
inequality, any word within `k` of the query differs from *this node* by between `d - k` and
`d + k`. **Every child edge outside that band is provably empty and never visited.**

Searching `chunck` with `k = 1`, starting at `chunk`: `d = 1`, so `chunk` is a hit, and we only
descend edges labelled `0`–`2`. The `5` branch — `storage` and everything under it — is skipped
without a single distance computation.

## Our implementation in Atlas

- **Where it lives:** `src/search/bk_tree.{h,cpp}` — `EditDistance` plus `BkTree::Insert` /
  `Search`. `SearchEngine::DidYouMean` drives it.
- **Two-row DP**, with the shorter string on the inner loop, so memory is `O(min(n,m))` rather
  than the full matrix.
- **Fed surface forms, not stems** — a correction has to be a word the user recognizes, same
  reasoning as autocomplete.
- **Silent when the word is already indexed.** `DidYouMean` returns nothing for a correctly
  spelled word: there's nothing to correct, and suggesting alternatives to a valid query is
  noise.
- **Default `max_distance = 2`**, the usual choice: it catches the overwhelming majority of real
  typos while keeping the candidate set small. At `k = 3` the pruning weakens sharply and
  unrelated words start appearing.
- **Results are nearest-first**, alphabetical on ties, so output is deterministic.

```cpp
const std::size_t distance = EditDistance(word, node->word);
if (distance <= max_distance) found.push_back({node->word, distance});

// Triangle inequality: only children whose edge label lies in [d-k, d+k] can hold a match.
const std::size_t low  = distance > max_distance ? distance - max_distance : 0;
const std::size_t high = distance + max_distance;
for (auto it = node->children.lower_bound(low); it != node->children.end() && it->first <= high;
     ++it) {
  stack.push_back(it->second.get());
}
```

`std::map` children make that band a `lower_bound` plus an in-order walk.

## Complexity & trade-offs

- **`EditDistance`:** `O(n · m)` time, `O(min(n,m))` space.
- **`Insert`:** one distance computation per level, so `O(depth)` computations.
- **`Search`:** worst case visits every node (a pathological tree, or a large `k`), but in
  practice touches a small fraction — empirically ~5–15% of nodes for `k ≤ 2`. There is **no
  proven sublinear bound**; the pruning is a strong heuristic, not a guarantee.
- **Tree shape depends on insertion order.** The first word inserted becomes the root forever,
  and a poor root gives a lopsided tree with weaker pruning. There is no rebalancing.
- **What we gave up:** exactness of *ranking*. Edit distance treats all edits equally, so it
  doesn't know that `hashign` is a transposition (one keystroke error) rather than two unrelated
  substitutions.

## Failure modes & edge cases

- **Empty tree / empty word** — both return no suggestions rather than crashing.
- **Duplicate inserts** are ignored (distance 0 to an existing node).
- **Large `k` degenerates** to near-exhaustive search, since the `[d-k, d+k]` band covers most
  edges. `k` must stay small for the structure to earn its keep.
- **Short words are dangerous** — at `k = 2`, a three-letter word is within range of a great many
  others, so corrections get noisy. Production systems scale `k` with word length.
- **Frequency is ignored.** Between two equidistant candidates we pick alphabetically, not by
  which is more common — a real ranker would weight by corpus frequency and keyboard adjacency.
- **Transpositions cost 2**, not 1. `Damerau`-Levenshtein counts a swap as a single edit and
  matches human typing better; we implement plain Levenshtein.
- **Deleted documents' words stop being suggested**: a delete decrements the vocabulary and the
  tree is rebuilt lazily on the next lookup ([incremental-indexing](incremental-indexing.md)).
  A BK-tree has no removal operation — deleting an interior node would orphan its whole subtree —
  so a rebuild is the only correct repair.
- **Byte-oriented**, so multibyte UTF-8 characters count as several edits.

## Alternatives we considered

- **Linear scan of the vocabulary** — simplest and exactly correct; the baseline our test
  compares the pruned search against. Too slow interactively at real vocabulary sizes.
- **Levenshtein automaton (Lucene's `FuzzyQuery`)** — build a DFA accepting everything within `k`
  edits of the query, then intersect it with the term dictionary's FST. Faster and with real
  guarantees, but substantially more machinery.
- **Deletion neighbourhoods (SymSpell)** — precompute every string obtainable by deleting up to
  `k` characters; lookup becomes hash probes and is very fast, at the cost of a large precomputed
  index that grows sharply with `k`.
- **N-gram overlap** — index character trigrams and rank candidates by shared trigrams. Cheap and
  approximate; a good *filter* before exact distance, but not a distance measure itself.
- **Phonetic keys (Soundex/Metaphone)** — catch sound-alike misspellings that edit distance
  misses, and vice versa. Complementary, not a replacement.

## Interview Q&A

**Q: Why can't a trie or hash map do fuzzy matching?**
Both need the beginning of the key to be right. A hash map needs exact equality; a trie needs a
correct prefix — and a typo in the first character defeats it. Nearness under edit distance
isn't lexicographic nearness, so you need an index over the metric.

**Q: What makes a BK-tree work?**
Edit distance is a metric, so the triangle inequality holds. After measuring the query against a
node, any match within `k` must sit on a child edge labelled within `[d-k, d+k]` — everything
else is provably empty and skipped unvisited.

**Q: What's the complexity of a BK-tree search?**
No proven sublinear bound. Worst case is every node; in practice a small fraction for `k ≤ 2`.
The pruning is a strong heuristic whose effectiveness depends on `k` and on the tree's shape.

**Q: How do you compute Levenshtein distance in linear space?**
Each DP cell depends only on the previous row and the current row's left neighbour, so you keep
two rows and swap — `O(min(n,m))` memory instead of the full matrix.

**Q: Why is `k = 2` the usual limit?**
It covers most real typos, and beyond it the `[d-k, d+k]` band stops excluding much, so pruning
collapses toward a full scan while the suggestions get noisier.

**Q: How would you rank several equidistant candidates?**
By corpus or query-log frequency first, then by keyboard adjacency and by treating transpositions
as single edits (Damerau-Levenshtein) — all closer to how people actually mistype than raw edit
distance.

## References

- Levenshtein, *Binary codes capable of correcting deletions, insertions, and reversals* (1966).
- Burkhard & Keller, *Some approaches to best-match file searching* (1973) — the BK-tree.
- Schulz & Mihov, *Fast String Correction with Levenshtein-Automata* (2002) — Lucene's approach.
- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*, ch. 3 — spelling
  correction.

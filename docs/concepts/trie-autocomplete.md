# Trie Autocomplete

**Area:** Search engine  ·  **Phase:** 3b  ·  **Status:** drafted

## TL;DR

A trie stores words as *paths of characters*, so every word sharing a prefix lives in one
subtree. That makes "what starts with `repl`?" a walk of four nodes followed by a traversal of
only the matching subtree — independent of how large the vocabulary is. Atlas ranks the
completions it finds by how often each word occurs, so `repl` returns `replication` first.

## The problem it solves

Autocomplete needs the set of words beginning with a prefix. Two obvious structures can't do it:

- **A hash map** destroys the information the question is about. Hashing deliberately scatters
  similar keys, so `replica` and `replication` land nowhere near each other. Answering the query
  means scanning every key — `O(vocabulary)` per keystroke.
- **A sorted array** does better: binary search to the first match, then read forward while the
  prefix holds (`O(log n + matches)`). Genuinely viable — but insertion is `O(n)` because it
  shifts elements, which is awkward for an index that grows continuously.

A trie answers the prefix question structurally: walking the prefix's characters *is* the
lookup, and everything below the node you land on is by construction a completion.

## How it works

Each edge is a character; each node is the prefix spelled by the path to it. Nodes that end a
real word carry its frequency.

```
        (root)
         /  \
        r    c
       /      \
      e        h
     /          \
    p            u
   /              \
  l                n
 /                 \
i                   k
|                   |
c ── a*(9)          *(54)
|                    \
a*(16)                s*(23)
|
t
|
i
|
o
|
n*(35)

  * = end of a word, with its frequency
```

`Complete("repl")` walks r→e→p→l — four steps, regardless of vocabulary size — then collects
every marked node beneath: `replica`, `replic`, `replication`.

**Prefixes are shared exactly once.** `replica`, `replicate` and `replication` store the letters
`replic` a single time, which is where the memory savings come from on a real vocabulary.

### Ranking the completions

A prefix can have hundreds of completions and the box shows five, so which five matters. Atlas
sorts by **term frequency**, breaking ties alphabetically for determinism:

```
"repl"    -> replication (35), replic (16), replicated (14), replica (9)
"chun"    -> chunk (54), chunks (23), chunking (5), chunked (2)
```

Frequency is a decent proxy for "what the user probably means" in a single-corpus search box. A
production system would blend in query logs, recency and personalization.

## Our implementation in Atlas

- **Where it lives:** `src/search/trie.{h,cpp}`. `SearchEngine::Suggest` calls it; the
  `SearchService.Suggest` RPC exposes it.
- **`std::map<char, unique_ptr<Node>>` for children** rather than a 256-entry array. The array
  is faster but allocates 256 pointers per node — mostly empty — while an ordered map keeps
  memory proportional to the branching actually present *and* makes the DFS emit words
  alphabetically for free, which gives us the tie-break with no extra sorting.
- **Fed with surface forms, not stems.** The index is keyed by stems, but suggesting `replic` to
  a user would be nonsense. `Analyze()` carries the original word alongside the stem
  ([tokenization-stemming](tokenization-stemming.md)) precisely so autocomplete has real words
  to offer. This is the one place the pipeline's lossy step has to be undone.
- **Top-K via `partial_sort`** over the collected completions, so a prefix with many matches
  doesn't pay a full sort.
- Insert is idempotent on frequency: re-inserting a word updates its count rather than
  duplicating it.

```cpp
std::vector<Completion> Trie::Complete(std::string_view prefix, std::size_t limit) const {
  const Node* node = &root_;
  for (const char ch : prefix) {                 // walk the prefix once
    const auto it = node->children.find(ch);
    if (it == node->children.end()) return {};   // no word starts with it
    node = it->second.get();
  }
  std::vector<Completion> found;
  std::string buffer(prefix);
  Collect(node, &buffer, &found);                // everything below is a completion
  // ... rank by frequency, keep `limit`
}
```

## Complexity & trade-offs

- **Insert:** `O(len(word) · log Σ)` — one map probe per character (`Σ` = distinct children).
- **Complete:** `O(len(prefix) · log Σ)` to walk, then `O(matches)` to collect and
  `O(matches · log limit)` to rank. **Independent of vocabulary size**, which is the whole point.
- **Space:** `O(total characters)` worst case, much less in practice because shared prefixes are
  stored once. Still heavier per word than a plain sorted array — a node with a map has real
  overhead, so tries trade memory for prefix-query speed.
- **What we gave up:** a prefix like `""` or `a` can match a huge subtree, so collection cost is
  bounded by matches, not by the limit. Production tries store the best completion per subtree
  so top-K can prune instead of enumerating.

## Failure modes & edge cases

- **Unknown prefix** returns empty immediately at the first missing character.
- **Empty prefix** matches the entire vocabulary — collection walks every node. Our demo uses it
  deliberately to show global top words, but it's the worst case.
- **A word is its own completion** — `Complete("ring")` includes `ring`.
- **Case and punctuation** are handled upstream by the tokenizer; the trie is byte-oriented and
  would treat `Ring` as a different word if fed unnormalized input.
- **Non-ASCII** — nodes key on bytes, so a multibyte UTF-8 character spans several levels. It
  round-trips correctly but a "prefix" can land mid-character, matching the tokenizer's
  documented ASCII-only limitation.
- **Deleted documents leave their words behind** — see
  [incremental-indexing](incremental-indexing.md); the vocabulary isn't decremented, so a
  suggestion can point at a word no live document contains.

## Alternatives we considered

- **Sorted array + binary search** — simpler, less memory, `O(log n + matches)` lookup. Loses on
  incremental insertion (`O(n)` shifts), which an index that grows document-by-document does
  constantly.
- **Ternary search tree** — much less memory than an array-of-children trie with similar
  behaviour; more complex, and `std::map` children already give us the memory profile.
- **Radix / Patricia trie** — collapses single-child chains into one edge, a big win for long
  shared prefixes like ours (`replic…`). The natural optimization if the trie gets large.
- **FST (finite-state transducer)** — what Lucene actually uses: a minimized automaton sharing
  suffixes as well as prefixes, giving dramatically smaller indexes. Immutable once built, so it
  suits a segment-based design better than an incrementally mutated one.
- **N-gram index for infix search** — answers "contains" rather than "starts with"; a different
  question, and much larger.

## Interview Q&A

**Q: Why not a hash map for autocomplete?**
Hashing scatters similar keys deliberately, so prefix locality is destroyed and answering a
prefix query means scanning every key. A trie makes the prefix walk *be* the lookup.

**Q: What's the complexity, and why does it matter that vocabulary size is absent from it?**
`O(len(prefix))` to locate the subtree plus `O(matches)` to collect. Autocomplete fires on every
keystroke, so the cost has to depend on what the user typed, not on how much you've indexed.

**Q: Why store surface forms rather than the stems the index uses?**
Because suggestions are shown to a human. The index matches `replic`; the user needs to see
`replication`. That's why the analyzer keeps both.

**Q: How do you choose which completions to show?**
Rank them — we use corpus term frequency with an alphabetical tie-break for determinism. Real
systems add query logs, recency and personalization.

**Q: How would you make top-K cheaper for a very short prefix?**
Store the best completion (or a max-frequency bound) at each node, then use a priority queue to
descend only into subtrees that could still contain a top-K result — pruning instead of
enumerating the whole subtree.

**Q: Array-of-children versus a map — when does each win?**
A fixed array is faster per step but allocates for every possible character at every node; a map
keeps memory proportional to real branching and yields alphabetical order for free. With a large
alphabet and sparse branching, the map wins.

## References

- Fredkin, *Trie Memory* (1960) — the original.
- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*, ch. 3 — dictionaries and
  wildcard/prefix queries.
- Lucene's `FST` implementation and Sedgewick & Wayne on ternary search trees.

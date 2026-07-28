# Tokenization & Stemming (the analysis pipeline)

**Area:** Search engine  ·  **Phase:** 3  ·  **Status:** drafted

## TL;DR

Before anything can be indexed or searched, raw text has to become a list of **terms**. Atlas's
pipeline is `tokenize → drop stop words → stem`: split text into lowercased alphanumeric tokens,
throw away high-frequency function words that carry no retrieval signal, and reduce each
remaining token to a common root so `replication`, `replicate`, and `replications` all collapse
to the single term `replic`. The same pipeline must run on documents *and* on queries — that
symmetry is what makes a match possible at all.

## The problem it solves

The naive approach is to store raw whitespace-separated words and match them literally. It
fails immediately, in four separate ways:

- **Case.** A document containing `Replication` doesn't match a query for `replication`.
- **Punctuation.** `fsync().` and `fsync` become different terms; `write-ahead` becomes one
  unsearchable blob.
- **Morphology.** A document about `replicating chunks` doesn't match a query for
  `chunk replication`, even though it's exactly what the user wanted. This is the big one — the
  user's vocabulary almost never matches the author's inflection.
- **Noise.** Words like `the`, `is`, `a` appear in nearly every document. They cost index space
  proportional to the corpus, and their [BM25](bm25.md) IDF is ~0 anyway, so they contribute
  nothing to ranking while dominating posting-list size.

Stemming attacks the third problem, which is the one you can't fix with a `tolower()` call.

## How it works

### Stage 1 — tokenization

Scan the byte stream; accumulate runs of alphanumeric characters, lowercasing as we go; every
other byte is a delimiter that terminates the current token.

```
"How does a filesystem journal writes?"
   ->  [how] [does] [a] [filesystem] [journal] [writes]
        0      1     2       3           4         5
```

Those indices matter — see *positions* below.

### Stage 2 — stop-word removal

Drop tokens in a fixed set of ~120 English function words. `how`, `does`, and `a` disappear;
`filesystem`, `journal`, `writes` survive.

### Stage 3 — stemming (Porter, 1980)

Porter's algorithm is **suffix stripping by rules**, not dictionary lookup. It's five ordered
phases of "if the word ends in X *and* the stem left behind is substantial enough, rewrite X".
Two definitions do all the work:

**Consonants and vowels.** A consonant is any letter other than `a/e/i/o/u`, *and* other than
`y` preceded by a consonant. So `y` is a **vowel** in `try` (preceded by consonant `r`) but a
**consonant** in `toy` (preceded by vowel `o`) and in `yes` (nothing precedes it). This
one-line rule is why `sky` and `happy` stem differently.

**The measure `m`.** Write the word as `[C](VC)^m[V]`, where `C` is a run of consonants and `V`
a run of vowels. `m` counts the VC groups — roughly "how many syllables of stem remain":

```
m=0:  tree   ->  C(tr) V(ee)                    — nothing after the vowel run
m=1:  trouble -> C(tr) V(ou) C(bl) V(e)
m=2:  private -> C(pr) V(i) C(v) V(a) C(t) V(e)
```

`m` is the guard that stops the algorithm from destroying short words. Stripping `-ate` from
`rate` would leave `r`, so the rule for `-ate` requires `m > 1` and simply doesn't fire.

**The five phases**, in order. The examples show *that phase's own rewrite* — later phases often
strip further, so the last column is not always the final term:

| Phase | Handles | That phase's rewrite |
|---|---|---|
| 1a | plurals | `caresses → caress`, `ponies → poni`, `cats → cat` |
| 1b | `-ed` / `-ing`, then repairs the stem | `hopping → hop`, `filing → file`, `agreed → agre` |
| 1c | terminal `y → i` | `happy → happi` |
| 2 | derivational suffixes | `relational → relate`, `predication → predicate` — both then reduced further by phase 3/4 to `relat`, `predic` |
| 3 | more derivational suffixes | `triplicate → triplic`, `hopeful → hope` |
| 4 | strips residual suffixes (`m > 1`) | `adjustment → adjust`, `connection → connect` |
| 5 | trailing `e`, doubled `l` | `probate → probat`, `controll → control` |

Porter's paper lists its examples per phase like this, which is a classic source of confusion:
the paper shows `relational → relate` under step 2, but the *stemmer's* answer for `relational`
is `relat`.

Phase 1b is the subtle one: after stripping `-ed`/`-ing` the stem is often malformed, so a
cleanup pass runs — restore a vowel (`at→ate`, `bl→ble`, `iz→ize`), un-double a final consonant
(`hopp → hop`), or add back a silent `e` when the stem ends consonant-vowel-consonant
(`fil → file`). That's why `hopping → hop` but `filing → file`.

### Worked example

`replications`, traced through every phase:

```
replications
  step 1a   ends "s"        -> replication
  step 1b   no -ed/-ing     -> replication
  step 1c   no terminal y   -> replication
  step 2    "ation" -> "ate", stem "replic" has m=2 > 0
                            -> replicate
  step 3    "icate" -> "ic", stem "repl" has m=1 > 0
                            -> replic
  step 4    ends "ic", but stem "repl" has m=1, rule needs m>1 — does not fire
                            -> replic
  step 5    no trailing e, no doubled l
                            -> replic   ← final term
```

And the payoff — every surface form reaches the same term:

```
replicate → replic    replication  → replic
replicated → replic   replications → replic
```

## Our implementation in Atlas

- **Where it lives:** `src/search/tokenizer.{h,cpp}` (stage 1), `src/search/stopwords.{h,cpp}`
  (stage 2), `src/search/stemmer.{h,cpp}` (stage 3, the `Porter` class), composed by
  `Analyze()` in `src/search/text_pipeline.{h,cpp}`. Tests in `tests/search/`.
- **Built by hand**, not pulled from a library — the algorithm *is* the learning goal. The only
  thing we'd ever take from a library here is boring text extraction (PDF/HTML).
- **`atlas_search` links nothing from `atlas_proto`.** The pipeline is deliberately free of gRPC
  so it can be built and tested against a local corpus while the storage track lands in
  parallel.

**The one non-obvious design decision: positions index the pre-filter token stream.**
`Analyze()` returns `{term, position}` pairs where `position` is the token's index *before* stop
words were removed:

```cpp
Analyze("chunks are replicated across the ring")
  -> [{chunk, 0}, {replic, 2}, {across, 3}, {ring, 5}]
                        ^                          ^
                 "are" dropped at 1        "the" dropped at 4
```

Renumbering densely (0,1,2,3) would be simpler and would be **wrong for phrase search**: the
gaps are what let a positional index (Phase 3b) distinguish `"chunk ring"` as an adjacent phrase
from a document where four words sit between them. Storing positions now is the same bet
[ADR-0007](../architecture/adr/0007-inverted-index-format-compression.md) makes about the
posting-list format — pay a little storage today to avoid re-indexing the whole corpus later.

**The invariant that matters:** documents and queries must go through the *identical* pipeline.
If the indexer stems but the query parser doesn't, a query for `replication` searches for a term
that appears nowhere in the index, and the engine silently returns nothing. Both paths call
`Analyze()`; there is no second implementation to drift.

```cpp
std::vector<Term> Analyze(std::string_view text) {
  const std::vector<std::string> tokens = Tokenize(text);
  std::vector<Term> terms;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (IsStopWord(tokens[i])) continue;
    terms.push_back(Term{Stem(tokens[i]), static_cast<std::uint32_t>(i)});
  }
  return terms;
}
```

## Complexity & trade-offs

- **Tokenization:** `O(n)` in bytes, single pass, no allocation beyond the tokens themselves.
- **Stop-word lookup:** `O(1)` average — a static `unordered_set<string_view>`, no allocation
  per lookup because the set holds views into string literals.
- **Stemming:** `O(1)` per token in practice. Each phase tests a bounded rule table against the
  suffix; `Measure()` is `O(len)` and runs a handful of times. No dictionary, no I/O, no state —
  which is exactly why Porter is still used 45 years on.
- **What we bought:** recall. `chunk replication` now finds documents that only ever say
  `replicating chunks`.
- **What we gave up:** precision, irreversibly. Stemming is lossy — `univers` cannot be turned
  back into `university` vs `universe`. The index no longer knows which word the author used.

## Failure modes & edge cases

- **Over-stemming (verified in our implementation):** `university` and `universe` both stem to
  `univers`; `organization` and `organ` both stem to `organ`. A query for one retrieves the
  other. This is inherent to rule-based stemming, not a bug in our port.
- **Under-stemming:** Porter only strips suffixes, so irregular forms never meet.
  `ran` stays `ran` while `running → run` — the two never match. A dictionary-based lemmatizer
  would fix this; a rule-based stemmer structurally cannot.
- **Non-word stems:** output isn't required to be a real word (`replic`, `distribut`,
  `consensus → consensu` — that last one is step 1a stripping the `-s` of a Latin `-us` ending).
  Harmless, because terms are only ever compared to other stemmed terms — but it means stems
  must **never** be shown to users as-is.
- **Stop-word-only queries return nothing.** `"to be or not to be"` analyzes to zero terms. A
  real system keeps stop words in a positional index to serve exactly this query; we accept the
  gap for M1.
- **Boolean operators vs. stop words:** `not` is in our stop-word list. Query *operators* must
  therefore be parsed **before** analysis, never recovered from the token stream afterwards.
- **Non-ASCII text.** `std::isalnum` is byte-oriented, so UTF-8 multibyte sequences split on
  their bytes instead of folding — accented and non-Latin text is effectively unsearchable.
  Documented limitation for M1; a real fix needs Unicode-aware segmentation and normalization
  (NFKC + case folding).
- **Empty input** at every stage yields an empty term list rather than an error; `Stem()` leaves
  words of ≤2 letters untouched, which also protects it from index underflow.

## Alternatives we considered

- **No stemming at all** — highest precision, badly hurt recall. The morphology mismatch is the
  single biggest source of "why did my search return nothing?"
- **Lemmatization (dictionary + part-of-speech)** — returns real words and handles irregulars
  (`ran → run`), so it's strictly more accurate. Needs a dictionary, POS tagging, and far more
  compute per token; a large dependency for a modest retrieval gain. Not for M1.
- **Snowball / Porter2** — Porter's own revision, fixing several known quirks. A drop-in upgrade
  later; we implemented the original because it's the one that's actually documented as a paper
  we can read end to end.
- **Lancaster / Krovetz** — more aggressive and more conservative respectively; neither is the
  standard baseline that Porter is.
- **Character n-grams** — language-agnostic and robust to typos (and would sidestep our
  ASCII limitation), but explodes index size and loses word boundaries.

## Interview Q&A

**Q: Why must documents and queries share one analysis pipeline?**
Because a term match is a *string* match on the post-analysis term. If the indexer stems
`replicating → replic` but the query path leaves `replication` alone, the query looks up a term
that doesn't exist in the index and returns nothing — silently. Symmetry isn't an optimization,
it's a correctness requirement.

**Q: What is Porter's measure `m` for, in one sentence?**
It's a guard on how much stem would survive a rewrite, expressed as the number of vowel-consonant
groups — it's what stops `-ate` from being stripped off `rate` while allowing it off `activate`.

**Q: Why does `hopping` stem to `hop` but `filing` to `file`?**
Both lose `-ing` in step 1b, leaving `hopp` and `fil`. The cleanup pass then un-doubles a final
consonant (`hopp → hop`) but adds a silent `e` back when the stem ends consonant-vowel-consonant
with `m=1` (`fil → file`).

**Q: Stemming vs. lemmatization?**
Stemming is rule-based suffix stripping — fast, stateless, and produces possibly-non-words
(`replic`). Lemmatization uses a dictionary and part-of-speech to produce real base forms and
handles irregulars (`ran → run`), at much higher cost. Stemming is the right default for lexical
retrieval; lemmatization matters when the output is shown to a human.

**Q: What's the cost of removing stop words?**
You save substantial index space and lose nothing in BM25 ranking (their IDF is near zero), but
you can no longer answer phrase queries made entirely of stop words (`"to be or not to be"`), and
you must parse boolean operators before analysis since words like `not` get dropped.

**Q: Why keep positions from before stop-word removal?**
So phrase search stays correct. Dense renumbering makes two terms look adjacent when a dropped
stop word actually sat between them, which would make `"chunk ring"` match text where it doesn't
appear as a phrase.

## References

- Porter, M.F., *An algorithm for suffix stripping*, Program 14(3), 1980 — the original paper;
  our rule tables and phase order come from its section 6.
- Manning, Raghavan & Schütze, *Introduction to Information Retrieval*, ch. 2 (tokenization,
  normalization, stemming) — the standard treatment, including the recall/precision trade-off.
- Snowball (snowballstem.org) — Porter's later revision and the reference implementations.

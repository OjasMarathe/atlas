# LRU & LFU Caches

**Area:** Query · **Phase:** 4 · **Status:** written

## TL;DR

A bounded cache has to answer one question: when it's full, *which entry leaves?* **LRU** evicts
the one untouched longest; **LFU** evicts the one requested least often. Both can be O(1) for
every operation including eviction — the interesting part of implementing them is making the
eviction decision without a scan. On Atlas's query traffic LFU wins by ~23 points of hit ratio,
because query popularity is a frequency property and the recent past is full of one-shot queries.

## The problem it solves

An uncached query fans out to every shard, twice (see [scatter-gather](scatter-gather.md)) — a
few milliseconds and `2 × shards` RPCs. Query traffic is heavily skewed: a small set of queries
repeats constantly while the long tail is seen once. Serving the repeats from memory turns
milliseconds into microseconds; in the Atlas demo, 11.4 ms → 0.01 ms.

The cache can't be unbounded, so the policy is the design. The naive LFU makes the mistake worth
naming: keep a counter per entry, and on eviction scan for the minimum. That's `O(n)` per miss —
the cache now costs more to maintain than the miss it's avoiding.

## How it works

### LRU in O(1)

A doubly-linked list in recency order (front = most recent) plus a hash map from key to that
key's **list node**.

```
map:  "a" ─┐   "b" ─┐   "c" ─┐
           ▼        ▼        ▼
list:   [ c ] <-> [ a ] <-> [ b ]        front = most recent, back = eviction victim
```

- **Get(k):** map lookup → node, then splice that node to the front. `splice` relinks pointers
  without moving the element, so the iterator the map stores stays valid — which is the only
  reason a map *of iterators* is safe at all.
- **Put(k,v) when full:** the victim is `list.back()`. No search.

### LFU in O(1)

The trick is bucketing by frequency instead of storing a bare count. Keep one list per frequency,
and track `min_frequency`.

```
buckets:  1 -> [ x ] <-> [ y ]        ← eviction comes from here (min_frequency = 1)
          2 -> [ z ]
          7 -> [ hot ]
entries:  "hot" -> {value, frequency=7, iterator into buckets[7]}
```

- **Get(k):** splice the node from `buckets[f]` to `buckets[f+1]`, `f += 1`. If that emptied
  `buckets[f]` and `f == min_frequency`, then `min_frequency = f + 1`.
- **Put** of a new key: insert at frequency 1 and set `min_frequency = 1`.
- **Evict:** `buckets[min_frequency].back()`.

**Why `min_frequency` can be maintained in O(1)** — the property the whole structure rests on: a
`Get` promotes exactly one entry, from `f` to `f+1`. The minimum can therefore only rise if that
entry was the *last* one at `f`, and when it rises it rises to exactly `f+1`. Any insertion resets
it to 1. So it never needs to be searched for.

Within a bucket, order is recency, so **ties at equal frequency break LRU**. Without that, a cache
full of equal-frequency entries evicts arbitrarily.

### Worked example — where they differ

Capacity 2. Access `a, b, a, a, b, [insert c]`.

| | counts | LRU order | victim |
|---|---|---|---|
| LRU | — | `b` newer than `a` | **`a`** — least recently used |
| LFU | `a`:3, `b`:2 | — | **`b`** — least frequently used |

LRU evicts the entry that has been requested *more* times, because one recent touch of `b`
outranks three older touches of `a`.

## Our implementation in Atlas

- **Where it lives:** [`src/common/cache/lru_cache.h`](../../src/common/cache/lru_cache.h),
  [`src/common/cache/lfu_cache.h`](../../src/common/cache/lfu_cache.h), behind the common
  interface in [`cache.h`](../../src/common/cache/cache.h). Header-only templates, so the
  coordinator caches `QueryResult` and a test can cache an `int`.
- **Swappable at runtime:** `QueryCoordinator` holds a `unique_ptr<Cache<...>>` chosen by
  `CoordinatorOptions::cache_policy` (`ATLAS_CACHE_POLICY=lru|lfu`). That is what makes the
  roadmap's "compare hit ratios" an experiment you can run rather than a claim.
- **Both are thread-safe.** Note that in a cache, `Get` *mutates* — it promotes the entry — so
  even reads take the exclusive lock and a `shared_mutex` would buy nothing.
- **Stats are part of the interface** (`hits`, `misses`, `evictions`, `hit_ratio()`), surfaced
  over gRPC as `CoordinatorService.CacheStats` and by `atlas cache`.

### The cache key

```cpp
query + "\x1f" + top_k + "\x1f" + (global_scoring ? "g" : "l")
```

`top_k` is the easy one to forget: without it, a cached top-3 would happily satisfy a later
request for top-10. The scoring mode matters for the same reason — the two modes produce
different rankings.

## Complexity & trade-offs

| Operation | LRU | LFU |
|---|---|---|
| Get | O(1) | O(1) |
| Put | O(1) | O(1) |
| Evict | O(1) | O(1) |
| Memory per entry | key + value + 2 pointers | key + value + 2 pointers + count |

LFU costs a little more memory and a second hash map (frequency → bucket). Its real cost is
conceptual: **LFU has no forgetting.** An entry that was hot last week keeps its count and can
squat in the cache long after it stopped being requested. Production LFUs add aging (halve all
counts periodically) or a window (TinyLFU). Atlas does neither yet — the cache is small and
process-lifetime, so a restart is the aging policy.

## Failure modes & edge cases

- **The cyclic-scan pathology.** When each pass touches more distinct keys than the cache holds,
  *in a fixed order*, every entry is evicted immediately before it is next needed. The hit ratio
  is not merely poor — it is **exactly zero**, and the cache is pure overhead. `cache_test` pins
  this down with 12 keys cycling through an 8-entry cache. Neither policy escapes it: LRU evicts
  by age, and LFU sees every key at frequency 1, so it degenerates to the same order. This is why
  the coordinator caches on the *query string*, where repeats are real, and nothing scan-shaped.
- **Cache stampede:** N concurrent misses for the same key all fan out. Atlas accepts this; the
  fix (single-flight, where the first miss takes a lock and the rest wait on its result) is worth
  it only under heavier load than M1 sees.
- **Staleness:** entries have no TTL and are not invalidated when a document is indexed, so a
  newly indexed document may not appear in a cached query's results. Honest gap, noted below.
- **Partial results are never cached** — see [scatter-gather](scatter-gather.md).
- **Capacity 0** is clamped to 1, so a cache that can hold nothing can't spin.

## Measured on Atlas's own workload

`cache_test` runs both policies over identical traffic — 8 hot keys requested 3× per round plus
10 one-shot keys, into a 16-entry cache:

```
skewed workload (8 hot x3, 10 one-shot, capacity 16):  LRU 47.1%,  LFU 70.5%
cyclic scan     (12 keys, capacity 8):                 LRU  0.0%,  LFU  0.0%
```

LFU wins by 23 points because at the end of every round the one-shot keys are the *most recent*
things in the cache. Recency says keep them; they are never requested again. Frequency correctly
identifies the working set. This is exactly the shape of search traffic, which is why LFU is worth
having even though LRU is the reflex choice.

## Alternatives we considered

- **FIFO** — evict oldest-inserted, ignoring access. Simplest, and meaningfully worse: it discards
  the hot set on schedule.
- **Random eviction** — surprisingly decent and lock-friendly, and what Redis's `allkeys-random`
  does. Rejected because the point of the phase is to implement and compare the two classic
  policies.
- **ARC / TinyLFU / W-TinyLFU** — adaptive policies that balance recency and frequency and solve
  LFU's no-forgetting problem. Strictly better in practice; deliberately out of scope, and the
  natural follow-up.
- **Redis** as an external cache — the roadmap floats it as a comparison. An out-of-process cache
  adds a network hop (~0.1 ms) against an in-process hit (~0.002 ms here); it earns that back only
  when the cache must be *shared* across coordinator instances, which is the point at which we'd
  add it.

## Interview Q&A

**Q: How do you make LFU eviction O(1)?**
Bucket entries by frequency (one list per count) and track `min_frequency`. Eviction is the back
of `buckets[min_frequency]`. `min_frequency` stays correct in O(1) because a promotion moves one
entry from `f` to `f+1`, so the minimum can only rise to `f+1` and only when `buckets[f]` empties;
insertion resets it to 1.

**Q: Why does the LRU map store list iterators?**
So a hit is O(1) end to end. `std::list::splice` moves a node to the front by relinking pointers
without invalidating iterators, so the stored iterator stays valid across promotions — that's the
property the design depends on.

**Q: When does LFU beat LRU, and when does it lose?**
LFU wins on skewed workloads with a stable working set and a stream of one-shot keys — recency is
actively misleading there, because the one-shots are the most recent things you have. LFU loses
when popularity *shifts*, since old counts keep stale entries resident; that's what aging or
TinyLFU fixes.

**Q: Is there a workload where both fail?**
Yes — a cyclic scan over more distinct keys than the cache holds gives exactly 0% for both. Every
entry is evicted just before it's needed again.

**Q: Why do reads take a write lock?**
Because a cache read isn't a read: `Get` promotes the entry, mutating the recency list or the
frequency bucket. A reader-writer lock would serialize identically while costing more.

## References

- Ketan Shah, Anirban Mitra, Dhruv Matani, *An O(1) algorithm for implementing the LFU cache
  eviction scheme* (2010) — the bucket construction used here.
- Megiddo & Modha, *ARC: A Self-Tuning, Low Overhead Replacement Cache* (FAST '03).
- Einziger, Friedman & Manes, *TinyLFU: A Highly Efficient Cache Admission Policy* (2017).
- [scatter-gather](scatter-gather.md) · [connection-pool](connection-pool.md)

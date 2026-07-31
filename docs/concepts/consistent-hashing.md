# Consistent Hashing

**Area:** Distributed file system  ·  **Phase:** 1  ·  **Status:** implemented + verified (flagship exemplar)

## TL;DR

Consistent hashing decides *which node stores which chunk* so that when a node joins or
leaves, only a small fraction (~1/N) of the data has to move — instead of almost all of it.
It maps both nodes and data keys onto the same circular hash space ("the ring") and assigns
each key to the next node clockwise. It's the backbone of Atlas's storage placement.

## The problem it solves

The naive way to spread K chunks over N nodes is `node = hash(chunk) % N`. It's perfectly
balanced — until N changes. Add or remove one node and the modulus changes for *almost every
key*, so nearly all data must be relocated. Example: with `% 4`, key hashing to 10 lands on
node `10 % 4 = 2`; switch to `% 5` and it moves to `10 % 5 = 0`. Do that for millions of
chunks and a single node joining triggers a near-total reshuffle — catastrophic for a
storage cluster.

We want a mapping where **changing N moves only the keys that *have* to move** (roughly the
share owned by the node that joined/left), and nothing else.

## How it works

Hash the key space onto a ring `[0, 2^m)` (we use the low 64 bits of SHA-256, so `2^64`).

1. **Place nodes on the ring.** Hash each node's id → a point on the ring.
2. **Place keys on the ring.** Hash each chunk id → a point on the ring.
3. **Assignment rule:** a key is owned by the **first node clockwise** from the key's point
   (i.e. the node with the smallest position ≥ the key's position, wrapping around).

```
        0 / 2^64
           ┌───── N_A (pos 10)
   N_D ────┤            k1(15) → first node clockwise = N_B
  (pos 55) │      k1(15)
           │            ┌── N_B (pos 30)
           │   k2(48)   │
           └── N_C ─────┘
             (pos 40)   k2(48) → first node clockwise = N_D
```

When **N_B leaves**, only keys in the arc `(N_A, N_B]` move — they shift to the next node
clockwise (N_C). Keys owned by N_A, N_C, N_D are untouched. When a node **joins**, it slots
into one arc and steals only that arc's keys from its clockwise successor. That's the whole
magic: **local churn instead of global churn.**

### Virtual nodes (vnodes) — the essential refinement

With one point per physical node, two problems appear: (a) load is uneven because random
points don't split the ring evenly, and (b) when a node dies, its *entire* load dumps onto a
single successor. Fix: give each physical node **V positions** on the ring (e.g. 100–200),
by hashing `node_id + "#" + i` for `i in [0, V)`. Now each node owns many small arcs
scattered around the ring, so:

- Load evens out (variance drops ~`1/√V`).
- A dead node's load spreads across *many* successors, not one.
- We can weight heterogeneous nodes by giving bigger nodes more vnodes.

## Our implementation in Atlas

- Lives in `src/common/hash_ring.{h,cpp}`; owned/served by the Metadata Service, consumed by
  Ingestion and Storage nodes for placement.
- **Ring = an ordered map** from ring position → node id: `std::map<uint64_t, NodeId>`
  (a balanced BST). Lookups are `upper_bound` (with wrap-around) → `O(log(N·V))`.
- **Hash:** SHA-256 of the id, take the first 8 bytes as a `uint64_t`. Uniform and stable.
- **Replication placement:** to place a chunk on 3 distinct nodes, start at the key's
  position and walk clockwise, collecting nodes, **skipping vnodes that map to a physical
  node we already picked**, until we have 3 distinct physical nodes. Those become
  primary / secondary / tertiary.

```cpp
// choose R distinct physical nodes for a key, clockwise from its hash position
std::vector<NodeId> HashRing::replicas(std::string_view key, int R) const {
    std::vector<NodeId> out;
    std::unordered_set<NodeId> seen;
    uint64_t h = hash64(key);
    auto it = ring_.upper_bound(h);              // first node clockwise
    for (size_t steps = 0; steps < ring_.size() && (int)out.size() < R; ++steps) {
        if (it == ring_.end()) it = ring_.begin();   // wrap around
        const NodeId& n = it->second;
        if (seen.insert(n).second) out.push_back(n); // skip repeat physical nodes
        ++it;
    }
    return out;                                   // [primary, secondary, tertiary]
}
```

Adding a node inserts its V vnodes into the map; removing one erases them. Only keys in the
affected arcs change owner — we recompute placement only for those chunks and migrate them.

**Verified** in `tests/hash_ring_test.cpp`: 3-distinct-replica placement, ~1/N load balance, and
the defining property — removing a node moves *only* that node's keys (`moved == owned_by_that_node`),
never everyone's.

## Complexity & trade-offs

| Operation | Cost |
|---|---|
| Lookup owner / replicas | `O(log(N·V))` per hop, R hops |
| Add / remove node | `O(V·log(N·V))` to update the map |
| Data moved on membership change | ~`1/N` of keys (× nice constant from vnodes) |
| Memory | `O(N·V)` ring entries |

Trade-off: more vnodes → better balance but a bigger ring and slightly slower lookups /
larger membership state. V≈100–200 is the usual sweet spot.

## Failure modes & edge cases

- **Hot keys:** consistent hashing balances *keys*, not *access*. A single wildly popular
  chunk still hammers its 3 replicas — needs caching/replication of hot items, not ring
  changes.
- **Correlated placement:** naive replica choice ("next 3 vnodes clockwise") can put 2
  replicas on the same physical node — hence the skip-seen-physical-node rule above.
- **Non-uniform hashing** would clump nodes; SHA-256 avoids this.
- **Ring disagreement:** if nodes hold different ring versions during a membership change,
  placement can diverge — the Metadata Service is the single source of truth for ring state
  and versions the ring so stale readers can be detected.

## Alternatives we considered

- **`hash % N`** — rejected: total reshuffle on membership change.
- **Rendezvous (HRW) hashing** — for each key, pick the node maximizing `hash(key,node)`.
  Also moves only ~1/N on change, needs no ring structure, and balances well; cost is
  `O(N)` per lookup vs our `O(log N·V)`. Great alternative; we chose the ring for its
  explicit, inspectable membership state and easy weighting via vnodes.
- **Jump consistent hash** — elegant and memory-free, but assumes numeric bucket ids and
  doesn't handle arbitrary node add/remove or weighting cleanly.

## Interview Q&A

**Q: Why not just `hash % N`?** Because changing N remaps almost every key; consistent
hashing remaps only ~1/N.

**Q: What do virtual nodes buy you?** Even load distribution and graceful failure spreading —
a dead node's data disperses across many successors instead of crushing one.

**Q: How do you place replicas without doubling up on one machine?** Walk the ring clockwise
from the key and skip vnodes belonging to physical nodes you've already selected.

**Q: Lookup complexity?** `O(log(N·V))` with an ordered map; `O(1)` amortized if you cache.

**Q: Where's the balancing limitation?** It balances key *ownership*, not request *load* — hot
keys still need caching/rebalancing on top.

## References

- Karger et al., *Consistent Hashing and Random Trees* (STOC 1997) — the original.
- DeCandia et al., *Dynamo: Amazon's Highly Available Key-value Store* (SOSP 2007) — vnodes
  in practice.
- Thaler & Ravishankar, *Rendezvous (HRW) Hashing* (1998).

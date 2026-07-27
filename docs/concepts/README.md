# Concepts

This folder is the point of Atlas. **One deep note per concept we implement.** Not a
paraphrase of a blog post — a note that explains the idea well enough that *we could teach
it*, tied to exactly how we built it in Atlas.

## The rule

> **A concept is not "done" until its note is written.** Every PR that implements a concept
> adds or updates its note in the same PR. Code and understanding land together.

Write the note *from* the code you just wrote, then sharpen it. If you can't explain a design
choice in the note, you don't understand it yet — which is the signal to go learn it.

## How to write one

Copy [`_TEMPLATE.md`](_TEMPLATE.md). Fill every section. Keep it concrete: reference the
actual files/functions in `src/`, include the real trade-offs we hit, and end with the
interview-style Q&A — this project is partly an interview-prep engine.

Two flagship notes set the bar: [consistent-hashing.md](consistent-hashing.md) and
[bm25.md](bm25.md). Match that depth.

## The concept checklist

Grouped by area; tagged with the phase where we hit it. Check off as notes land.

### Distributed file system & storage (Phase 1)
- [x] [consistent-hashing](consistent-hashing.md) — placing data on nodes so joins/leaves move little
- [x] [chunking](chunking.md) — fixed-size, content-addressed blocks
- [ ] replication — primary/secondary/tertiary placement & write path
- [ ] wal — write-ahead logging for crash consistency
- [x] [sha256-checksums](sha256-checksums.md) — integrity & content addressing
- [ ] versioning — copy-on-write file versions
- [ ] garbage-collection — reclaiming dead chunks/versions
- [ ] merkle-tree — efficient replica divergence detection *(stretch)*

### Fault tolerance & consensus (Phase 2)
- [ ] heartbeat-failure-detection — timeouts, phi-accrual (concept)
- [ ] replica-promotion — leader handoff for a chunk group
- [ ] self-healing — re-replication to restore the factor
- [ ] raft — leader election + replicated log *(stretch)*
- [ ] quorum-consistency — R/W quorums, why R+W>N

### Search engine (Phase 3)
- [ ] tokenization-stemming — text → normalized terms (+ stop-words)
- [ ] inverted-index — term → posting list; the core data structure
- [ ] posting-list-compression — delta + varint/gap encoding
- [x] [bm25](bm25.md) — the ranking function
- [ ] incremental-indexing — updating the index without full rebuilds
- [ ] trie-autocomplete — prefix completion
- [ ] bk-tree-levenshtein — fuzzy/spell-correct via edit distance
- [ ] boolean-phrase-search — AND/OR/NOT + positional phrase matching

### Distributed query engine (Phase 4)
- [ ] index-sharding — document- vs term-partitioning (ADR-0006)
- [ ] scatter-gather — fan-out, merge, global top-K
- [ ] lru-lfu-cache — eviction policies compared
- [ ] thread-pool — bounded worker pool + task queue
- [ ] connection-pool — reusing gRPC channels
- [ ] async-io — Boost.Asio event loop, back-pressure

### Semantic search (Phase 5)
- [ ] embeddings — sentence-transformer vectors
- [ ] vector-search — ANN / cosine similarity
- [ ] hybrid-ranking — fusing BM25 + vector scores

### Crawler (Phase 6)
- [ ] crawler-architecture — frontier, politeness, pipeline
- [ ] bloom-filter — probabilistic dedup
- [ ] rate-limiting — token bucket / leaky bucket
- [ ] robots-politeness — robots.txt, crawl-delay

### Security (Phase 7)
- [ ] jwt-auth · [ ] rbac-acl · [ ] encryption-at-rest · [ ] tls

### Observability & infra (Phases 8–9)
- [ ] metrics-prometheus · [ ] distributed-tracing · [ ] analytics-dashboard
- [ ] containerization · [ ] k8s-orchestration · [ ] chaos-testing · [ ] load-testing

> Not every stretch item will get built — but if we build it, it gets a note. If we
> deliberately skip one, say so in the relevant design doc and why.

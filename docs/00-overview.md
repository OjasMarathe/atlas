# Atlas — Overview

## Vision

A single search box over an organization's **entire** knowledge base — no matter that
the knowledge lives in PDFs, wikis, GitHub repos, drives, and research papers — backed
by storage and search infrastructure we built and understand end to end.

## The problem

Inside any large organization (a university like BITS, or a company), information is
scattered across incompatible silos. Each tool only searches its own island:

- Google Search doesn't index private files.
- Drive search is shallow and doesn't understand structure.
- GitHub search doesn't understand PDFs.
- Confluence only searches Confluence.

There is no unified, high-quality search over *everything the org knows*.

## What we're building

An **Enterprise Distributed Knowledge Platform**: a miniature, from-scratch distributed
cloud that (1) stores documents durably across a cluster, and (2) searches them fast and
well. Concretely, ten modules — a distributed file system, a crawler/ingestion pipeline,
a search engine, semantic search, a distributed query engine, fault tolerance, security,
analytics, monitoring, and infrastructure. See [plan/roadmap.md](plan/roadmap.md).

## Why we're building it

This is a **learning-first, resume-defining** project. Two explicit goals:

1. **Understand distributed systems by implementing the internals ourselves** — not by
   wiring together managed services. Every algorithm (consistent hashing, replication,
   Raft, inverted index, BM25, caches) is written by hand and *documented deeply* in
   [concepts/](concepts/). A concept is not "done" until its note is written.
2. **Ship something that genuinely runs distributed** — multiple nodes, real replication,
   real failure recovery — because a working distributed system is a rare, strong signal
   for a software-engineering role.

## Goals (what success looks like)

- **G1** — Documents are chunked, checksummed, versioned, and replicated 3× across nodes.
- **G2** — Losing a node loses no data and the cluster self-heals automatically.
- **G3** — Full-text search over the corpus with correct BM25 ranking, sub-second on the
  demo dataset.
- **G4** — Search is *distributed*: a coordinator fans out to shards and merges results.
- **G5** — Every concept we touch has a written note that could stand alone as a tutorial.
- **G6** — The repo reads like real infrastructure: clean architecture, ADRs, tests, CI,
  a code-review history.

## Non-goals (explicitly out of scope)

- Not competing with Elasticsearch / Google / Confluence on scale or features.
- Not multi-datacenter / geo-replication. One cluster, one region (conceptually).
- Not real petabyte scale — we design *as if* for it, demo at small scale.
- Not a polished product UI — the dashboard exists to visualize the system, not to ship.
- Not novel research — the value is depth of understanding and a working artifact.

## Success criteria for Milestone 1 (July 30)

The end-to-end demo in the [README](../README.md) runs live: **upload → chunk → replicate
→ distributed search → kill a node → self-heal → search still correct.** Everything past
that (semantic search, crawler, security, full observability, k8s) is a documented later
phase, not a July-30 requirement.

## Learning objectives (the concepts we owe ourselves)

Storage & distribution: chunking, metadata separation, consistent hashing, replication,
quorums, heartbeats & failure detection, self-healing, garbage collection, versioning,
WAL, checksums, Merkle trees, Raft/leader election.

Search & retrieval: tokenization, stemming, stop-words, inverted index, posting-list
compression, incremental indexing, BM25, tries, BK-trees, Levenshtein, phrase/boolean
queries, embeddings, vector search, hybrid ranking.

Systems: thread pools, producer-consumer, lock-free queues, C++ memory model & atomics,
LRU/LFU caches, connection pooling, async I/O, scatter-gather, back-pressure.

Each gets a note in [concepts/](concepts/).

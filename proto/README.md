# `proto/` — Service Contracts (the co-design keystone)

These `.proto` files are the **source of truth for every boundary** between Atlas services.
We agree on them *together first*; then the storage track and search track build behind them
in parallel without colliding. Change a contract → regenerate stubs → implement.

## Files

| File | Defines | Primary owner |
|---|---|---|
| `common.proto` | shared types: `NodeInfo`, `RingState`, `ChunkHandle`, `ChunkPlacement`, `Status` | both |
| `storage.proto` | `StorageService` — chunk put/get/delete, replicate, heartbeat | storage (Ojas) |
| `metadata.proto` | `MetadataService` — file map, ring/membership, versions | storage (Ojas) |
| `search.proto` | `SearchService` — index a doc, search a shard, suggest, stats | search (Harshal) |
| `coordinator.proto` | `CoordinatorService` — client-facing query fan-out | search (Harshal) |

## Conventions

- **proto3**, package `atlas`. Timestamps use `google.protobuf.Timestamp`.
- Chunks are content-addressed by the **hex SHA-256** of their bytes.
- Large chunk transfers **stream** `ChunkFrame`s (a chunk can exceed gRPC's 4 MB default limit).
- Prefer gRPC status codes on the wire; `Status` carries domain-specific detail.
- **`TODO(co-design):`** comments mark spots we should settle together before implementing.
- Never renumber existing fields; only add new ones (wire compatibility).

## Regenerating stubs

Handled by CMake at build time (protobuf + gRPC codegen). Generated `*.pb.*` / `*.grpc.pb.*`
files are **git-ignored** — never commit generated code. (Build wiring lands with the Phase 0
CMake skeleton.)

## Status

Draft v0 — **awaiting joint review** before we implement behind them. Argue here first.

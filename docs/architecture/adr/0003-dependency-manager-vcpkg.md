# ADR-0003 — C++ dependency manager: vcpkg

**Status:** Accepted (2026-07-21)

## Context

Atlas pulls in several non-trivial C++ dependencies — gRPC, Protobuf, RocksDB, Boost.Asio,
GoogleTest. These must build **reproducibly** on two developer machines (Ojas + Harshal) and in
CI, or we lose hours to "works on my machine." Per [ADR-0001](0001-language-cpp20.md), a real
dependency manager is a required mitigation for the C++ toolchain tax.

## Options considered

- **vcpkg (manifest mode)** — Microsoft. A `vcpkg.json` in the repo pins dependencies; tight
  CMake toolchain integration; binary caching to avoid rebuilding deps.
- **Conan** — Python-based, powerful profiles and remotes; more moving parts; excellent for
  complex cross-compilation we don't need.
- **System packages (brew/apt)** — fast to start, **not reproducible** across machines/CI.
- **Git submodules + build-from-source** — full control, but slow and manual.

## Decision

**vcpkg in manifest mode.** Commit `vcpkg.json` (deps) + a pinned `builtin-baseline`, and wire
vcpkg's toolchain file into CMake so every dev and CI resolves the identical dependency set.

## Consequences

- **Positive:** reproducible builds everywhere; trivial onboarding (`cmake` picks up the
  toolchain); CI caches vcpkg binaries so only the first build is slow.
- **Negative:** the **first build is slow** (deps compile from source unless a binary cache is
  warm); we must keep the baseline pinned and updated deliberately.
- Chosen over Conan for tighter CMake integration and a simpler manifest for a two-person team;
  we don't need Conan's advanced profile/remote features.

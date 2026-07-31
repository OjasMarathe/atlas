# SHA-256 & Checksums

**Area:** DFS / integrity · **Phase:** 1 · **Status:** written

## TL;DR

SHA-256 turns any bytes into a fixed 256-bit (64 hex-char) fingerprint. Atlas uses it two ways:
as a chunk's **content address** (its id *is* the SHA-256 of its bytes) and as an **integrity
check** (recompute on read; a mismatch means the data was corrupted or tampered with).

## The problem it solves

We need to (a) *name* chunks so identical content is stored once and is verifiable, and (b)
*detect* silent corruption — a disk bit-flip, a truncated transfer, a buggy node returning wrong
bytes. A cryptographic hash gives both: a deterministic, collision-resistant fingerprint of the
bytes.

## How it works

SHA-256 is a Merkle–Damgård construction:
1. **Pad** to a multiple of 512 bits: append a `1` bit, then zeros, then the original length as a
   64-bit big-endian integer.
2. Process each **512-bit block** through a compression function: expand 16 words into a 64-word
   **message schedule** `W`, then run **64 rounds** mixing eight working variables `a…h` with round
   constants `K`, the `Ch`/`Maj` functions, and bitwise rotations/shifts.
3. Add each block's output into the running 256-bit state `H[0..7]`; the final `H` is the digest.

Key properties: **deterministic**, **avalanche** (flip one input bit → ~half the output bits flip),
and **collision/preimage resistance** — which is what makes it safe as a content address.

## Our implementation in Atlas

- `src/common/sha256.{h,cpp}` — a streaming `Sha256` class (`Update`/`HexDigest`) + one-shot
  `Sha256Hex`. Written from scratch (the roadmap lists SHA-256 as a from-scratch algorithm —
  implementing it *is* the learning).
- **Verified** against FIPS 180-4 known-answer vectors in `tests/chunking_test.cpp`. This is
  non-negotiable for a hash: a subtly wrong implementation yields plausible-looking but wrong
  digests.
- Used by chunking (`chunk_id = Sha256Hex(bytes)`) and the read path (recompute + compare →
  mismatch reads another replica).

## Complexity & trade-offs

`O(n)` time in message length, constant memory (a 64-byte block buffer + 32-byte state). Streaming —
never holds more than one block beyond the input.

## Failure modes & edge cases

- **Padding / length bugs** are the classic error — always test against standard vectors.
- **Endianness**: SHA-256 is big-endian throughout (schedule + length field).
- **Not for passwords**: it's *fast*, which is bad for password hashing (use a slow KDF —
  bcrypt/scrypt/Argon2). Fast is exactly what we want for integrity/addressing.

## Alternatives we considered

- **MD5 / SHA-1** — broken (practical collisions); unsafe as content addresses.
- **CRC32** — fast but only catches *accidental* corruption, not collisions/tampering.
- **BLAKE3 / xxHash** — faster and modern; BLAKE3 is a fine future swap. We chose SHA-256 for
  ubiquity, a stable standard, and because GFS-style systems use it.

## Interview Q&A

**Q: Why hash chunks instead of sequential IDs?** Content addressing gives dedup, integrity, and
immutability for free.
**Q: What stops two chunks colliding?** SHA-256's collision resistance (~2^128 work to break).
**Q: Why not MD5?** Practical collisions exist — an attacker could forge a chunk with the same address.
**Q: Fine for passwords?** No — too fast; use a deliberately slow KDF.

## References

- FIPS PUB 180-4, *Secure Hash Standard*.
- Ghemawat et al., *The Google File System* (SOSP 2003) — checksummed chunks.

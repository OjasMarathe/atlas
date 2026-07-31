// Dependency-free tests for piece #1 (SHA-256 + chunking).
// Runs standalone via clang++ AND as a CMake/CTest target — no gRPC or GoogleTest needed.

#include <cstdio>
#include <string>
#include <vector>

#include "common/sha256.h"
#include "dfs/chunking.h"

namespace {
int g_checks = 0;
int g_fails = 0;
}  // namespace

#define CHECK_EQ(a, b)                                             \
  do {                                                             \
    ++g_checks;                                                    \
    if (!((a) == (b))) {                                           \
      ++g_fails;                                                   \
      std::printf("FAIL (line %d): %s == %s\n", __LINE__, #a, #b); \
    }                                                              \
  } while (0)

#define CHECK(cond)                                         \
  do {                                                      \
    ++g_checks;                                             \
    if (!(cond)) {                                          \
      ++g_fails;                                            \
      std::printf("FAIL (line %d): %s\n", __LINE__, #cond); \
    }                                                       \
  } while (0)

int main() {
  using namespace atlas;

  // --- SHA-256 known-answer vectors (FIPS 180-4) ---
  CHECK_EQ(Sha256Hex(std::string("")),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(Sha256Hex(std::string("abc")),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(Sha256Hex(std::string("The quick brown fox jumps over the lazy dog")),
           std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));

  // Streaming update must equal the one-shot digest.
  {
    Sha256 s;
    s.Update("ab", 2);
    s.Update("c", 1);
    CHECK_EQ(s.HexDigest(), Sha256Hex(std::string("abc")));
  }

  // --- chunking: exact multiple + remainder + content addressing ---
  {
    const std::string data(10, 'x');
    auto chunks = ChunkBytes(data, 4);  // 4 + 4 + 2
    CHECK_EQ(chunks.size(), size_t(3));
    CHECK_EQ(chunks[0].data.size(), size_t(4));
    CHECK_EQ(chunks[2].data.size(), size_t(2));
    CHECK_EQ(Reassemble(chunks), data);                 // roundtrip
    CHECK_EQ(chunks[0].id, Sha256Hex(chunks[0].data));  // id is the content address
    CHECK_EQ(chunks[0].id, chunks[1].id);               // identical content -> identical id
  }

  // Empty input -> no chunks, empty roundtrip.
  {
    auto chunks = ChunkBytes(std::string(""), 4);
    CHECK(chunks.empty());
    CHECK_EQ(Reassemble(chunks), std::string(""));
  }

  // Default 4 MiB boundary: one full chunk + a small remainder.
  {
    const std::string data(kChunkSize + 100, 'y');
    auto chunks = ChunkBytes(data);
    CHECK_EQ(chunks.size(), size_t(2));
    CHECK_EQ(chunks[0].data.size(), kChunkSize);
    CHECK_EQ(chunks[1].data.size(), size_t(100));
    CHECK_EQ(Reassemble(chunks), data);
  }

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

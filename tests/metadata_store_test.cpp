// Tests for the versioned MetadataStore (piece #3). Links proto + RocksDB; runs via CMake/CTest.
// Verifies copy-on-write versioning, latest/specific lookups, list, and persistence.

#include <cstdio>
#include <filesystem>
#include <string>

#include "metadata/metadata_store.h"

namespace {
int g_checks = 0;
int g_fails = 0;
}  // namespace

#define CHECK(cond)                                         \
  do {                                                      \
    ++g_checks;                                             \
    if (!(cond)) {                                          \
      ++g_fails;                                            \
      std::printf("FAIL (line %d): %s\n", __LINE__, #cond); \
    }                                                       \
  } while (0)

#define CHECK_EQ(a, b)                                             \
  do {                                                             \
    ++g_checks;                                                    \
    if (!((a) == (b))) {                                           \
      ++g_fails;                                                   \
      std::printf("FAIL (line %d): %s == %s\n", __LINE__, #a, #b); \
    }                                                              \
  } while (0)

int main() {
  using namespace atlas;
  namespace fs = std::filesystem;
  const std::string dbpath = (fs::temp_directory_path() / "atlas_metadata_test").string();
  fs::remove_all(dbpath);

  {
    MetadataStore store(dbpath);
    CHECK(store.ok());

    // RegisterFile assigns incrementing versions per file_id, with timestamps.
    FileMetadata a1;
    a1.set_file_id("fileA");
    a1.set_owner("ojas");
    a1.add_chunks()->mutable_chunk()->set_chunk_id("c1");
    const FileMetadata r1 = store.RegisterFile(a1);
    CHECK_EQ(r1.version(), 1u);
    CHECK(r1.has_created_at());
    CHECK(r1.has_modified_at());

    FileMetadata a2;
    a2.set_file_id("fileA");
    a2.add_chunks()->mutable_chunk()->set_chunk_id("c2");
    CHECK_EQ(store.RegisterFile(a2).version(), 2u);

    // Independent version counter per file.
    FileMetadata b1;
    b1.set_file_id("fileB");
    CHECK_EQ(store.RegisterFile(b1).version(), 1u);

    // GetFile: latest (version 0) and a specific version, with chunk lists intact.
    FileMetadata got;
    CHECK(store.GetFile("fileA", 0, &got));
    CHECK_EQ(got.version(), 2u);
    CHECK_EQ(got.chunks(0).chunk().chunk_id(), std::string("c2"));

    CHECK(store.GetFile("fileA", 1, &got));
    CHECK_EQ(got.version(), 1u);
    CHECK_EQ(got.chunks(0).chunk().chunk_id(), std::string("c1"));
    CHECK_EQ(got.owner(), std::string("ojas"));

    // Absent file.
    CHECK(!store.GetFile("nope", 0, &got));

    // ListVersions ascending.
    const auto vers = store.ListVersions("fileA");
    CHECK_EQ(vers.size(), size_t(2));
    CHECK_EQ(vers[0].version(), 1u);
    CHECK_EQ(vers[1].version(), 2u);
  }

  // Persistence: reopen, latest survives, and the version sequence continues.
  {
    MetadataStore store(dbpath);
    FileMetadata got;
    CHECK(store.GetFile("fileA", 0, &got));
    CHECK_EQ(got.version(), 2u);

    FileMetadata a3;
    a3.set_file_id("fileA");
    CHECK_EQ(store.RegisterFile(a3).version(), 3u);
  }

  fs::remove_all(dbpath);

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

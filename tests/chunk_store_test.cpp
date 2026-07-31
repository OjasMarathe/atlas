// Tests for the RocksDB-backed ChunkStore (piece #2). Links RocksDB, so it runs via CMake/CTest
// (not standalone clang++). Verifies persistence, checksum-on-write, checksum-on-read, delete.

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "common/sha256.h"
#include "storage/chunk_store.h"

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
  const std::string dbpath = (fs::temp_directory_path() / "atlas_chunkstore_test").string();
  fs::remove_all(dbpath);  // clean slate

  const std::string data = "hello atlas chunk";
  const std::string id = Sha256Hex(data);

  // --- put / get / exists / delete ---
  {
    ChunkStore store(dbpath);
    CHECK(store.ok());

    CHECK(store.Put(id, data) == StoreStatus::kOk);
    CHECK(store.Exists(id));

    std::string out;
    CHECK(store.Get(id, &out) == StoreStatus::kOk);
    CHECK_EQ(out, data);

    // Wrong id (id != sha256(data)) is refused on write.
    CHECK(store.Put("deadbeef", data) == StoreStatus::kChecksumMismatch);

    // Missing chunk -> not found.
    CHECK(store.Get(Sha256Hex(std::string("absent")), &out) == StoreStatus::kNotFound);

    // Delete removes it.
    CHECK(store.Delete(id) == StoreStatus::kOk);
    CHECK(!store.Exists(id));
    CHECK(store.Get(id, &out) == StoreStatus::kNotFound);
  }

  // --- durability: data survives a close + reopen ---
  {
    ChunkStore store(dbpath);
    CHECK(store.Put(id, data) == StoreStatus::kOk);
  }
  {
    ChunkStore store(dbpath);  // reopen the same path
    std::string out;
    CHECK(store.Get(id, &out) == StoreStatus::kOk);
    CHECK_EQ(out, data);
  }

  // --- corruption on read is caught ---
  // Inject bytes whose hash != key by writing directly to RocksDB (bypassing Put's check),
  // then verify ChunkStore::Get rejects them.
  {
    fs::remove_all(dbpath);
    {
      std::unique_ptr<rocksdb::DB> raw;
      rocksdb::Options opt;
      opt.create_if_missing = true;
      CHECK(rocksdb::DB::Open(opt, dbpath, &raw).ok());
      rocksdb::WriteOptions wo;
      wo.sync = true;
      CHECK(raw->Put(wo, id, "CORRUPTED BYTES").ok());  // stored under id, but wrong content
      raw.reset();                                      // close so ChunkStore can open it
    }
    ChunkStore store(dbpath);
    std::string out;
    CHECK(store.Get(id, &out) == StoreStatus::kChecksumMismatch);
  }

  fs::remove_all(dbpath);

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

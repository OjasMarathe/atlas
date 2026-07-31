#ifndef ATLAS_STORAGE_CHUNK_STORE_H_
#define ATLAS_STORAGE_CHUNK_STORE_H_

#include <memory>
#include <string>

namespace rocksdb {
class DB;
}

namespace atlas {

enum class StoreStatus { kOk, kNotFound, kChecksumMismatch, kError };

// Persistent, content-addressed chunk store backed by RocksDB. A chunk is keyed by its SHA-256
// id; writes are durable (WAL fsync before returning) and reads verify the checksum, so silent
// corruption is caught. See docs/concepts/wal.md and docs/concepts/sha256-checksums.md.
class ChunkStore {
 public:
  // Opens (creating if needed) a store at `path`. Check ok() afterwards.
  explicit ChunkStore(const std::string& path);
  ~ChunkStore();

  ChunkStore(const ChunkStore&) = delete;
  ChunkStore& operator=(const ChunkStore&) = delete;

  bool ok() const { return db_ != nullptr; }

  // Store `data` under `chunk_id`. Rejects with kChecksumMismatch if SHA256(data) != chunk_id.
  // Durable (sync write) before returning.
  StoreStatus Put(const std::string& chunk_id, const std::string& data);

  // Fetch a chunk into `*out`; verifies SHA256(bytes) == chunk_id (returns kChecksumMismatch on
  // corruption, kNotFound if absent).
  StoreStatus Get(const std::string& chunk_id, std::string* out) const;

  StoreStatus Delete(const std::string& chunk_id);
  bool Exists(const std::string& chunk_id) const;

 private:
  std::unique_ptr<rocksdb::DB> db_;
};

}  // namespace atlas

#endif  // ATLAS_STORAGE_CHUNK_STORE_H_

#ifndef ATLAS_METADATA_METADATA_STORE_H_
#define ATLAS_METADATA_METADATA_STORE_H_

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "metadata.pb.h"

namespace rocksdb {
class DB;
}

namespace atlas {

// Persistent, versioned file-metadata store (RocksDB). Each RegisterFile creates a new immutable
// version (copy-on-write); GetFile(version = 0) returns the latest. See
// docs/concepts/versioning.md.
//
// Thread-safe. Several of these operations are read-modify-write sequences over a single key —
// allocating the next version number, merging holders into the chunk-location index — and
// RocksDB's own per-operation atomicity does not make those atomic. Since Phase 2 there are two
// independent writers on the metadata node: the gRPC service threads and the maintenance loop's
// healer, which holds a raw `MetadataStore*` and never goes through MetadataService. Guarding
// inside the store is therefore the only place that covers both.
class MetadataStore {
 public:
  explicit MetadataStore(const std::string& path);
  ~MetadataStore();
  MetadataStore(const MetadataStore&) = delete;
  MetadataStore& operator=(const MetadataStore&) = delete;

  bool ok() const { return db_ != nullptr; }

  // Assigns the next version for meta.file_id() (+ timestamps), persists it, returns the stored
  // copy.
  FileMetadata RegisterFile(FileMetadata meta);

  // version == 0 -> latest. Returns false if the file/version is absent.
  bool GetFile(const std::string& file_id, uint64_t version, FileMetadata* out) const;

  // All versions of a file, ascending by version number.
  std::vector<FileMetadata> ListVersions(const std::string& file_id) const;

  // ---- chunk location index (Phase 2) ----
  //
  // Who currently holds each chunk. Unlike a file *version* — immutable history — locations are
  // mutable: self-healing adds a holder, a node loss removes one. Keeping them out of the version
  // blob is what lets GetFile serve *current* placements without rewriting immutable history, and
  // it dedups naturally, since one content-addressed chunk can be shared by many files/versions.
  void AddChunkLocation(const std::string& chunk_id, const std::string& node_id);
  void RemoveChunkLocation(const std::string& chunk_id, const std::string& node_id);
  std::vector<std::string> ChunkLocations(const std::string& chunk_id) const;
  std::vector<std::string> AllChunkIds() const;  // every chunk the cluster knows about

 private:
  // Private helpers never lock: every public entry point already holds mutex_, and re-entering a
  // non-recursive mutex would deadlock.
  uint64_t LatestVersion(const std::string& file_id) const;  // 0 if none
  ChunkPlacement LoadLocations(const std::string& chunk_id) const;
  void FillLiveLocations(FileMetadata* meta) const;  // placements <- location index

  // Shared for readers, exclusive for the read-modify-write paths. Reads outnumber writes and a
  // write ends in a synchronous (fsync-ing) RocksDB commit, so it is worth not serializing them.
  mutable std::shared_mutex mutex_;
  std::unique_ptr<rocksdb::DB> db_;
};

}  // namespace atlas

#endif  // ATLAS_METADATA_METADATA_STORE_H_

#ifndef ATLAS_METADATA_METADATA_STORE_H_
#define ATLAS_METADATA_METADATA_STORE_H_

#include <cstdint>
#include <memory>
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

 private:
  uint64_t LatestVersion(const std::string& file_id) const;  // 0 if none

  std::unique_ptr<rocksdb::DB> db_;
};

}  // namespace atlas

#endif  // ATLAS_METADATA_METADATA_STORE_H_

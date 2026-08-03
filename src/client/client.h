#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "common/hash_ring.h"  // NodeId
#include "dfs/chunking.h"
#include "metadata.grpc.pb.h"
#include "search.grpc.pb.h"
#include "storage.grpc.pb.h"

namespace atlas {

struct UploadResult {
  bool ok = false;
  std::string message;
  std::size_t chunks = 0;
  bool indexed = false;  // the document was also handed to a search shard
};

struct UploadOptions {
  // Index the payload for search after it is stored. Skipped automatically for data that is not
  // text — an inverted index over a JPEG's bytes is noise, and Atlas has no extractor yet.
  bool index = true;
  std::map<std::string, std::string> fields;  // author/title/type, for Phase 3b filters
};

struct DownloadResult {
  bool ok = false;
  std::string data;
  std::string message;
};

// Client-side ingestion + read path. Upload chunks a file, places each chunk on R nodes chosen by
// the consistent-hashing ring, writes them, acks at write-quorum W (ADR-0004), then commits the
// file in the metadata service. Download reads the chunk map back and reassembles, reading around
// any dead replica. Talks to the metadata service + storage nodes over gRPC.
// See docs/architecture/module-01-dfs.md.
class AtlasClient {
 public:
  static constexpr int kReplicationFactor = 3;
  static constexpr int kWriteQuorum = 2;  // W=2 of N=3: survives one node loss, no write stall

  // chunk_size is configurable so tests can drive the multi-chunk path without multi-MiB files.
  explicit AtlasClient(const std::string& metadata_address, std::size_t chunk_size = kChunkSize);

  UploadResult Upload(const std::string& file_id, const std::string& owner, const std::string& data,
                      const UploadOptions& options = {});
  DownloadResult Download(const std::string& file_id, std::uint64_t version = 0);

 private:
  StorageService::Stub* StorageAt(const std::string& address);  // channels cached by address
  SearchService::Stub* SearchAt(const std::string& address);
  bool FetchRing(RingState* out);

  // Hands the document to the shard that owns it (ADR-0006: document-partitioned, and a
  // document's owner is the primary holder of its first chunk). Returns false if no candidate
  // shard accepted it.
  bool IndexDocument(const std::vector<NodeId>& owners,
                     const std::unordered_map<std::string, std::string>& address,
                     const std::string& file_id, const std::string& text,
                     const std::map<std::string, std::string>& fields);

  std::unique_ptr<MetadataService::Stub> metadata_;
  std::size_t chunk_size_ = kChunkSize;
  std::unordered_map<std::string, std::unique_ptr<StorageService::Stub>> storage_;
  std::unordered_map<std::string, std::unique_ptr<SearchService::Stub>> search_;
};

}  // namespace atlas

#pragma once

#include <string>

#include "storage.grpc.pb.h"

namespace atlas {

// Streaming chunk transfer over StorageService, shared by the ingestion client and the Phase 2
// healer so there is exactly one implementation of the framing rules (header frame first, then
// <=1 MiB data frames — a chunk can exceed gRPC's default 4 MB message limit).

// Stores `data` under `chunk_id` on the node behind `stub`. True only if the node durably
// accepted it (its own checksum check passed).
bool PutChunk(StorageService::Stub* stub, const std::string& chunk_id, const std::string& data,
              int timeout_seconds = 2);

// Same, over the ReplicateChunk RPC — used when the caller is repairing/replicating rather than
// performing an original write.
bool ReplicateChunk(StorageService::Stub* stub, const std::string& chunk_id,
                    const std::string& data, int timeout_seconds = 5);

// Fetches a chunk into `*out`. False means "try another replica": the node is unreachable, the
// chunk is absent, or its checksum failed.
bool GetChunk(StorageService::Stub* stub, const std::string& chunk_id, std::string* out,
              int timeout_seconds = 2);

// Drops a chunk from a node. Used when rebalancing moves a replica elsewhere — only ever after
// the new copy is durable.
bool DeleteChunk(StorageService::Stub* stub, const std::string& chunk_id, int timeout_seconds = 2);

}  // namespace atlas

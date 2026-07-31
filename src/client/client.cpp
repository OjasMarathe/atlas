#include "client/client.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include "common/hash_ring.h"
#include "common/sha256.h"
#include "dfs/chunking.h"

namespace atlas {
namespace {

// Client-streamed PutChunk: one header frame then <=1 MiB data frames. Returns true only if the
// node durably stored the chunk (its own checksum check passed).
bool PutChunkToNode(StorageService::Stub* stub, const std::string& chunk_id,
                    const std::string& data) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
  PutChunkResponse response;
  std::unique_ptr<grpc::ClientWriter<ChunkFrame>> writer(stub->PutChunk(&ctx, &response));

  ChunkFrame header;
  header.mutable_header()->set_chunk_id(chunk_id);
  header.mutable_header()->set_size(data.size());
  if (!writer->Write(header)) {
    writer->Finish();
    return false;
  }
  constexpr std::size_t kFrame = 1u << 20;
  for (std::size_t off = 0; off < data.size(); off += kFrame) {
    ChunkFrame frame;
    frame.set_data(data.substr(off, std::min(kFrame, data.size() - off)));
    if (!writer->Write(frame)) {
      writer->Finish();
      return false;
    }
  }
  writer->WritesDone();
  return writer->Finish().ok() && response.status().code() == Status::OK;
}

// Server-streamed GetChunk: drain the frames into `out`. The node verifies the checksum and
// returns a non-OK status on corruption/absence, so a false return means "try another replica".
bool GetChunkFromNode(StorageService::Stub* stub, const std::string& chunk_id, std::string* out) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
  GetChunkRequest request;
  request.set_chunk_id(chunk_id);
  request.set_verify_checksum(true);
  std::unique_ptr<grpc::ClientReader<ChunkFrame>> reader(stub->GetChunk(&ctx, request));

  std::string data;
  ChunkFrame frame;
  while (reader->Read(&frame)) {
    if (frame.has_data()) data += frame.data();
  }
  if (!reader->Finish().ok()) return false;
  *out = std::move(data);
  return true;
}

}  // namespace

AtlasClient::AtlasClient(const std::string& metadata_address, std::size_t chunk_size)
    : metadata_(MetadataService::NewStub(
          grpc::CreateChannel(metadata_address, grpc::InsecureChannelCredentials()))),
      chunk_size_(chunk_size) {}

StorageService::Stub* AtlasClient::StorageAt(const std::string& address) {
  auto it = storage_.find(address);
  if (it == storage_.end()) {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    it = storage_.emplace(address, StorageService::NewStub(channel)).first;
  }
  return it->second.get();
}

bool AtlasClient::FetchRing(RingState* out) {
  grpc::ClientContext ctx;
  GetRingRequest request;
  return metadata_->GetRing(&ctx, request, out).ok();
}

UploadResult AtlasClient::Upload(const std::string& file_id, const std::string& owner,
                                 const std::string& data) {
  RingState ring_state;
  if (!FetchRing(&ring_state)) return {false, "failed to fetch ring", 0};
  if (ring_state.nodes_size() < kReplicationFactor) {
    return {false, "cluster has fewer nodes than the replication factor", 0};
  }

  // Rebuild the ring locally (deterministic, so placements match every node's view) and map
  // node id -> address for connecting.
  HashRing ring(static_cast<int>(ring_state.virtual_nodes_per_node()));
  std::unordered_map<std::string, std::string> address;
  for (const NodeInfo& node : ring_state.nodes()) {
    ring.AddNode(node.node_id());
    address[node.node_id()] = node.address();
  }

  RegisterFileRequest reg;
  reg.set_file_id(file_id);
  reg.set_owner(owner);
  reg.set_replication_factor(kReplicationFactor);
  reg.set_sha256(Sha256Hex(data));

  const std::vector<Chunk> chunks = ChunkBytes(data, chunk_size_);
  for (const Chunk& chunk : chunks) {
    const std::vector<NodeId> replicas = ring.Replicas(chunk.id, kReplicationFactor);
    ChunkPlacement* placement = reg.add_chunks();
    placement->mutable_chunk()->set_chunk_id(chunk.id);
    placement->mutable_chunk()->set_size(chunk.data.size());

    // Record ONLY nodes that durably acked, so the placement list is the truth about who holds
    // the bytes. Recording every *intended* replica would make an under-replicated chunk look
    // healthy forever: Phase 2's healer compares this list against ring.Replicas(chunk_id, R) —
    // intended vs actual — and that difference is exactly what it must repair.
    int acks = 0;
    for (const NodeId& node : replicas) {
      if (PutChunkToNode(StorageAt(address[node]), chunk.id, chunk.data)) {
        placement->add_node_ids(node);
        ++acks;
      }
    }
    if (acks < kWriteQuorum) {
      return {false, "chunk " + chunk.id + " did not reach write quorum (W=2)", 0};
    }
  }

  grpc::ClientContext ctx;
  FileMetadata committed;
  const grpc::Status status = metadata_->RegisterFile(&ctx, reg, &committed);  // commit point
  if (!status.ok()) return {false, "RegisterFile failed: " + status.error_message(), 0};
  return {true, "", chunks.size()};
}

DownloadResult AtlasClient::Download(const std::string& file_id, std::uint64_t version) {
  FileMetadata meta;
  {
    grpc::ClientContext ctx;
    GetFileRequest request;
    request.set_file_id(file_id);
    request.set_version(version);
    const grpc::Status status = metadata_->GetFile(&ctx, request, &meta);
    if (!status.ok()) return {false, {}, "GetFile failed: " + status.error_message()};
  }

  RingState ring_state;
  if (!FetchRing(&ring_state)) return {false, {}, "failed to fetch ring"};
  std::unordered_map<std::string, std::string> address;
  for (const NodeInfo& node : ring_state.nodes()) address[node.node_id()] = node.address();

  std::string data;
  for (const ChunkPlacement& placement : meta.chunks()) {
    std::string bytes;
    bool got = false;
    for (const std::string& node : placement.node_ids()) {
      const auto it = address.find(node);
      if (it == address.end()) continue;  // node left the cluster
      if (GetChunkFromNode(StorageAt(it->second), placement.chunk().chunk_id(), &bytes)) {
        got = true;
        break;  // read-around: first healthy replica wins
      }
    }
    if (!got) return {false, {}, "no healthy replica for chunk " + placement.chunk().chunk_id()};
    data += bytes;
  }

  if (Sha256Hex(data) != meta.sha256()) return {false, {}, "whole-file checksum mismatch"};
  return {true, std::move(data), ""};
}

}  // namespace atlas

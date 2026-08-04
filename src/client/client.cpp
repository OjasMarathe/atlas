#include "client/client.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include "common/hash_ring.h"
#include "common/sha256.h"
#include "dfs/chunking.h"
#include "storage/chunk_transfer.h"  // shared streaming helpers (also used by the Phase 2 healer)

namespace atlas {

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

SearchService::Stub* AtlasClient::SearchAt(const std::string& address) {
  auto it = search_.find(address);
  if (it == search_.end()) {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    it = search_.emplace(address, SearchService::NewStub(channel)).first;
  }
  return it->second.get();
}

namespace {

// Cheap "is this text?" test. A NUL byte is the reliable tell for binary in practice, and the
// prefix is enough — we are deciding whether to index, not validating an encoding. A real
// pipeline extracts text from PDFs and DOCXs here; that is Phase 6's crawler/extractor work.
bool LooksLikeText(const std::string& data) {
  if (data.empty()) return false;
  const std::size_t sample = std::min<std::size_t>(data.size(), 8192);
  for (std::size_t i = 0; i < sample; ++i) {
    if (data[i] == '\0') return false;
  }
  return true;
}

}  // namespace

bool AtlasClient::IndexDocument(const std::vector<NodeId>& owners,
                                const std::unordered_map<std::string, std::string>& address,
                                const std::string& file_id, const std::string& text,
                                const std::map<std::string, std::string>& fields) {
  IndexDocumentRequest request;
  request.set_file_id(file_id);
  request.set_text(text);
  for (const auto& [key, value] : fields) (*request.mutable_fields())[key] = value;

  // Exactly one shard indexes the document (ADR-0006), so the coordinator's merge sees it once.
  // The candidates are its chunk's holders in ring-preference order; we walk them only until one
  // accepts, which makes the owner deterministic while still tolerating a dead primary.
  for (const NodeId& owner : owners) {
    const auto it = address.find(owner);
    if (it == address.end()) continue;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    Status response;
    if (SearchAt(it->second)->IndexDocument(&context, request, &response).ok()) return true;
  }
  return false;
}

bool AtlasClient::FetchRing(RingState* out) {
  grpc::ClientContext ctx;
  GetRingRequest request;
  return metadata_->GetRing(&ctx, request, out).ok();
}

UploadResult AtlasClient::Upload(const std::string& file_id, const std::string& owner,
                                 const std::string& data, const UploadOptions& options) {
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
  std::vector<NodeId> document_owners;  // holders of chunk 0, in ring-preference order
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
      if (PutChunk(StorageAt(address[node]), chunk.id, chunk.data)) {
        placement->add_node_ids(node);
        ++acks;
      }
    }
    if (acks < kWriteQuorum) {
      return {false, "chunk " + chunk.id + " did not reach write quorum (W=2)", 0};
    }
    if (document_owners.empty()) document_owners = replicas;
  }

  grpc::ClientContext ctx;
  FileMetadata committed;
  const grpc::Status status = metadata_->RegisterFile(&ctx, reg, &committed);  // commit point
  if (!status.ok()) return {false, "RegisterFile failed: " + status.error_message(), 0};

  // Index *after* the commit, and never fail the upload on it. The bytes are durable and the
  // file is readable at this point; a shard being unreachable makes the document temporarily
  // unsearchable, which is a far smaller problem than rejecting a write that already landed.
  bool indexed = false;
  if (options.index && LooksLikeText(data)) {
    indexed = IndexDocument(document_owners, address, file_id, data, options.fields);
  }
  return {true, "", chunks.size(), indexed};
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
      if (GetChunk(StorageAt(it->second), placement.chunk().chunk_id(), &bytes)) {
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

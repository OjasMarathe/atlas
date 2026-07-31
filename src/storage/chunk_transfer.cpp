#include "storage/chunk_transfer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>

#include <grpcpp/grpcpp.h>

namespace atlas {
namespace {

constexpr std::size_t kFrameSize = 1u << 20;  // 1 MiB data frames

void SetDeadline(grpc::ClientContext* ctx, int seconds) {
  ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(seconds));
}

// Writes header + data frames into an already-open client stream.
bool WriteFrames(grpc::ClientWriterInterface<ChunkFrame>* writer, const std::string& chunk_id,
                 const std::string& data) {
  ChunkFrame header;
  header.mutable_header()->set_chunk_id(chunk_id);
  header.mutable_header()->set_size(data.size());
  if (!writer->Write(header)) return false;
  for (std::size_t off = 0; off < data.size(); off += kFrameSize) {
    ChunkFrame frame;
    frame.set_data(data.substr(off, std::min(kFrameSize, data.size() - off)));
    if (!writer->Write(frame)) return false;
  }
  return true;
}

}  // namespace

bool PutChunk(StorageService::Stub* stub, const std::string& chunk_id, const std::string& data,
              int timeout_seconds) {
  grpc::ClientContext ctx;
  SetDeadline(&ctx, timeout_seconds);
  PutChunkResponse response;
  std::unique_ptr<grpc::ClientWriter<ChunkFrame>> writer(stub->PutChunk(&ctx, &response));
  if (!WriteFrames(writer.get(), chunk_id, data)) {
    writer->Finish();
    return false;
  }
  writer->WritesDone();
  return writer->Finish().ok() && response.status().code() == Status::OK;
}

bool ReplicateChunk(StorageService::Stub* stub, const std::string& chunk_id,
                    const std::string& data, int timeout_seconds) {
  grpc::ClientContext ctx;
  SetDeadline(&ctx, timeout_seconds);
  PutChunkResponse response;
  std::unique_ptr<grpc::ClientWriter<ChunkFrame>> writer(stub->ReplicateChunk(&ctx, &response));
  if (!WriteFrames(writer.get(), chunk_id, data)) {
    writer->Finish();
    return false;
  }
  writer->WritesDone();
  return writer->Finish().ok() && response.status().code() == Status::OK;
}

bool DeleteChunk(StorageService::Stub* stub, const std::string& chunk_id, int timeout_seconds) {
  grpc::ClientContext ctx;
  SetDeadline(&ctx, timeout_seconds);
  DeleteChunkRequest request;
  request.set_chunk_id(chunk_id);
  Status response;
  return stub->DeleteChunk(&ctx, request, &response).ok() && response.code() == Status::OK;
}

bool GetChunk(StorageService::Stub* stub, const std::string& chunk_id, std::string* out,
              int timeout_seconds) {
  grpc::ClientContext ctx;
  SetDeadline(&ctx, timeout_seconds);
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

}  // namespace atlas

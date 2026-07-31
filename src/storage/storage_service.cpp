#include "storage/storage_service.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace atlas {

grpc::Status StorageServiceImpl::ReceiveAndStore(grpc::ServerReader<ChunkFrame>* reader,
                                                 PutChunkResponse* response) {
  std::string chunk_id;
  std::string data;
  bool have_header = false;

  ChunkFrame frame;
  while (reader->Read(&frame)) {
    if (frame.has_header()) {
      chunk_id = frame.header().chunk_id();
      have_header = true;
    } else if (frame.has_data()) {
      data += frame.data();
    }
  }

  auto* st = response->mutable_status();
  if (!have_header) {
    st->set_code(Status::INTERNAL);
    st->set_message("missing header frame");
    return grpc::Status::OK;
  }

  switch (store_->Put(chunk_id, data)) {
    case StoreStatus::kOk:
      st->set_code(Status::OK);
      response->mutable_stored()->set_chunk_id(chunk_id);
      response->mutable_stored()->set_size(data.size());
      break;
    case StoreStatus::kChecksumMismatch:
      st->set_code(Status::CHECKSUM_MISMATCH);
      st->set_message("sha256(data) != chunk_id");
      break;
    default:
      st->set_code(Status::INTERNAL);
      st->set_message("store error");
  }
  return grpc::Status::OK;
}

grpc::Status StorageServiceImpl::PutChunk(grpc::ServerContext*,
                                          grpc::ServerReader<ChunkFrame>* reader,
                                          PutChunkResponse* response) {
  return ReceiveAndStore(reader, response);
}

grpc::Status StorageServiceImpl::ReplicateChunk(grpc::ServerContext*,
                                                grpc::ServerReader<ChunkFrame>* reader,
                                                PutChunkResponse* response) {
  // For M1 a replica just stores locally; primary-driven fan-out is piece #5.
  return ReceiveAndStore(reader, response);
}

grpc::Status StorageServiceImpl::GetChunk(grpc::ServerContext*, const GetChunkRequest* request,
                                          grpc::ServerWriter<ChunkFrame>* writer) {
  std::string data;
  switch (store_->Get(request->chunk_id(), &data)) {
    case StoreStatus::kOk:
      break;
    case StoreStatus::kNotFound:
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "no such chunk");
    case StoreStatus::kChecksumMismatch:
      return grpc::Status(grpc::StatusCode::DATA_LOSS, "checksum mismatch on read");
    default:
      return grpc::Status(grpc::StatusCode::INTERNAL, "store error");
  }

  ChunkFrame header;
  header.mutable_header()->set_chunk_id(request->chunk_id());
  header.mutable_header()->set_size(data.size());
  writer->Write(header);

  // Split into <4 MiB frames so a full chunk never exceeds gRPC's default message limit.
  constexpr size_t kFrameSize = 1u << 20;  // 1 MiB
  for (size_t off = 0; off < data.size(); off += kFrameSize) {
    ChunkFrame df;
    df.set_data(data.substr(off, std::min(kFrameSize, data.size() - off)));
    writer->Write(df);
  }
  return grpc::Status::OK;
}

grpc::Status StorageServiceImpl::DeleteChunk(grpc::ServerContext*,
                                             const DeleteChunkRequest* request, Status* response) {
  const bool ok = store_->Delete(request->chunk_id()) == StoreStatus::kOk;
  response->set_code(ok ? Status::OK : Status::INTERNAL);
  return grpc::Status::OK;
}

grpc::Status StorageServiceImpl::Heartbeat(grpc::ServerContext*, const HeartbeatRequest*,
                                           HeartbeatResponse* response) {
  response->mutable_status()->set_code(Status::OK);
  response->set_ring_version(0);
  return grpc::Status::OK;
}

}  // namespace atlas

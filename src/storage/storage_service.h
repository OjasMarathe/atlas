#ifndef ATLAS_STORAGE_STORAGE_SERVICE_H_
#define ATLAS_STORAGE_STORAGE_SERVICE_H_

#include <grpcpp/grpcpp.h>

#include "storage.grpc.pb.h"
#include "storage/chunk_store.h"

namespace atlas {

// gRPC StorageService backed by a local ChunkStore: chunk put/get/delete/replicate + heartbeat.
// See docs/architecture/module-01-dfs.md. Does not own the ChunkStore.
class StorageServiceImpl final : public StorageService::Service {
 public:
  explicit StorageServiceImpl(ChunkStore* store) : store_(store) {}

  grpc::Status PutChunk(grpc::ServerContext* context, grpc::ServerReader<ChunkFrame>* reader,
                        PutChunkResponse* response) override;
  grpc::Status GetChunk(grpc::ServerContext* context, const GetChunkRequest* request,
                        grpc::ServerWriter<ChunkFrame>* writer) override;
  grpc::Status DeleteChunk(grpc::ServerContext* context, const DeleteChunkRequest* request,
                           Status* response) override;
  grpc::Status ReplicateChunk(grpc::ServerContext* context, grpc::ServerReader<ChunkFrame>* reader,
                              PutChunkResponse* response) override;
  grpc::Status Heartbeat(grpc::ServerContext* context, const HeartbeatRequest* request,
                         HeartbeatResponse* response) override;

 private:
  // Shared by PutChunk + ReplicateChunk: reassemble the streamed frames and store the chunk.
  grpc::Status ReceiveAndStore(grpc::ServerReader<ChunkFrame>* reader, PutChunkResponse* response);

  ChunkStore* store_;
};

}  // namespace atlas

#endif  // ATLAS_STORAGE_STORAGE_SERVICE_H_

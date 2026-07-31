// gRPC integration test for StorageServiceImpl over a real loopback server (piece #2 layer 2).
// Starts an actual gRPC server on an ephemeral port and drives it through a client stub, so the
// streaming put/get path is verified end to end.

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "common/sha256.h"
#include "storage.grpc.pb.h"
#include "storage/chunk_store.h"
#include "storage/storage_service.h"

namespace {
int g_checks = 0;
int g_fails = 0;
}  // namespace

#define CHECK(cond)                                         \
  do {                                                      \
    ++g_checks;                                             \
    if (!(cond)) {                                          \
      ++g_fails;                                            \
      std::printf("FAIL (line %d): %s\n", __LINE__, #cond); \
    }                                                       \
  } while (0)

#define CHECK_EQ(a, b)                                             \
  do {                                                             \
    ++g_checks;                                                    \
    if (!((a) == (b))) {                                           \
      ++g_fails;                                                   \
      std::printf("FAIL (line %d): %s == %s\n", __LINE__, #a, #b); \
    }                                                              \
  } while (0)

int main() {
  using namespace atlas;
  namespace fs = std::filesystem;
  const std::string dbpath = (fs::temp_directory_path() / "atlas_storage_svc_test").string();
  fs::remove_all(dbpath);

  ChunkStore store(dbpath);
  CHECK(store.ok());
  StorageServiceImpl svc(&store);

  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&svc);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  CHECK(server != nullptr);
  CHECK(port != 0);

  auto channel =
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
  auto stub = StorageService::NewStub(channel);

  const std::string data = "networked chunk over grpc";
  const std::string id = Sha256Hex(data);

  // PutChunk (client streaming): header frame + data frame.
  {
    grpc::ClientContext ctx;
    PutChunkResponse resp;
    auto writer = stub->PutChunk(&ctx, &resp);
    ChunkFrame hdr;
    hdr.mutable_header()->set_chunk_id(id);
    hdr.mutable_header()->set_size(data.size());
    CHECK(writer->Write(hdr));
    ChunkFrame df;
    df.set_data(data);
    CHECK(writer->Write(df));
    writer->WritesDone();
    CHECK(writer->Finish().ok());
    CHECK(resp.status().code() == Status::OK);
    CHECK_EQ(resp.stored().chunk_id(), id);
  }

  // GetChunk (server streaming): reassemble the frames back into the original bytes.
  {
    GetChunkRequest req;
    req.set_chunk_id(id);
    grpc::ClientContext ctx;
    auto reader = stub->GetChunk(&ctx, req);
    std::string got;
    std::string got_id;
    ChunkFrame f;
    while (reader->Read(&f)) {
      if (f.has_header()) {
        got_id = f.header().chunk_id();
      } else if (f.has_data()) {
        got += f.data();
      }
    }
    CHECK(reader->Finish().ok());
    CHECK_EQ(got_id, id);
    CHECK_EQ(got, data);
  }

  // A put whose id doesn't match its bytes is rejected over the wire.
  {
    grpc::ClientContext ctx;
    PutChunkResponse resp;
    auto writer = stub->PutChunk(&ctx, &resp);
    ChunkFrame hdr;
    hdr.mutable_header()->set_chunk_id("deadbeef");
    writer->Write(hdr);
    ChunkFrame df;
    df.set_data(data);
    writer->Write(df);
    writer->WritesDone();
    writer->Finish();
    CHECK(resp.status().code() == Status::CHECKSUM_MISMATCH);
  }

  // DeleteChunk, then GetChunk returns NOT_FOUND.
  {
    DeleteChunkRequest dreq;
    dreq.set_chunk_id(id);
    Status dresp;
    grpc::ClientContext ctx;
    CHECK(stub->DeleteChunk(&ctx, dreq, &dresp).ok());
    CHECK(dresp.code() == Status::OK);

    GetChunkRequest greq;
    greq.set_chunk_id(id);
    grpc::ClientContext ctx2;
    auto reader = stub->GetChunk(&ctx2, greq);
    ChunkFrame f;
    while (reader->Read(&f)) {
    }
    CHECK(reader->Finish().error_code() == grpc::StatusCode::NOT_FOUND);
  }

  server->Shutdown();
  fs::remove_all(dbpath);

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

// atlas — command-line client for a running Atlas cluster.
//
//   atlas put  <file_id> <path>        upload a file
//   atlas get  <file_id> [out_path]    download it (stdout if no path)
//   atlas info <file_id>               show chunks and which nodes hold them
//   atlas nodes                        show cluster membership
//
// Env: ATLAS_METADATA (default 127.0.0.1:50050)

#include <grpcpp/grpcpp.h>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "client/client.h"
#include "metadata.grpc.pb.h"

namespace {

std::string MetadataAddress() {
  const char* v = std::getenv("ATLAS_METADATA");
  return (v != nullptr && *v != '\0') ? std::string(v) : std::string("127.0.0.1:50050");
}

int Usage() {
  std::cerr << "usage:\n"
            << "  atlas put  <file_id> <path>\n"
            << "  atlas get  <file_id> [out_path]\n"
            << "  atlas info <file_id>\n"
            << "  atlas nodes\n";
  return 2;
}

int Put(const std::string& file_id, const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "cannot read " << path << "\n";
    return 1;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string data = std::move(buffer).str();

  atlas::AtlasClient client(MetadataAddress());
  const atlas::UploadResult result =
      client.Upload(file_id, std::getenv("USER") != nullptr ? std::getenv("USER") : "atlas", data);
  if (!result.ok) {
    std::cerr << "upload failed: " << result.message << "\n";
    return 1;
  }
  std::cout << "uploaded " << file_id << " (" << data.size() << " bytes, " << result.chunks
            << " chunk(s), replicated 3x)\n";
  return 0;
}

int Get(const std::string& file_id, const std::string& out_path) {
  atlas::AtlasClient client(MetadataAddress());
  const atlas::DownloadResult result = client.Download(file_id);
  if (!result.ok) {
    std::cerr << "download failed: " << result.message << "\n";
    return 1;
  }
  if (out_path.empty()) {
    std::cout << result.data;
  } else {
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
      std::cerr << "cannot write " << out_path << "\n";
      return 1;
    }
    out << result.data;
    std::cout << "wrote " << result.data.size() << " bytes to " << out_path << "\n";
  }
  return 0;
}

std::unique_ptr<atlas::MetadataService::Stub> MetadataStub() {
  return atlas::MetadataService::NewStub(
      grpc::CreateChannel(MetadataAddress(), grpc::InsecureChannelCredentials()));
}

int Info(const std::string& file_id) {
  auto stub = MetadataStub();
  grpc::ClientContext ctx;
  atlas::GetFileRequest request;
  request.set_file_id(file_id);
  atlas::FileMetadata meta;
  const grpc::Status status = stub->GetFile(&ctx, request, &meta);
  if (!status.ok()) {
    std::cerr << "not found: " << file_id << " (" << status.error_message() << ")\n";
    return 1;
  }
  std::cout << file_id << "  version " << meta.version() << "  owner " << meta.owner() << "  "
            << meta.chunks_size() << " chunk(s)\n";
  for (const atlas::ChunkPlacement& placement : meta.chunks()) {
    std::cout << "  " << placement.chunk().chunk_id().substr(0, 12) << "…  " << std::setw(9)
              << placement.chunk().size() << " B  holders:";
    for (const std::string& node : placement.node_ids()) std::cout << " " << node;
    std::cout << "\n";
  }
  return 0;
}

int Nodes() {
  auto stub = MetadataStub();
  grpc::ClientContext ctx;
  atlas::GetRingRequest request;
  atlas::RingState state;
  if (!stub->GetRing(&ctx, request, &state).ok()) {
    std::cerr << "cannot reach metadata at " << MetadataAddress() << "\n";
    return 1;
  }
  std::cout << "ring version " << state.version() << ", " << state.nodes_size() << " node(s), "
            << state.virtual_nodes_per_node() << " vnodes each\n";
  for (const atlas::NodeInfo& node : state.nodes()) {
    std::cout << "  " << node.node_id() << "  " << node.address() << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return Usage();

  const std::string& command = args[0];
  if (command == "put" && args.size() == 3) return Put(args[1], args[2]);
  if (command == "get" && (args.size() == 2 || args.size() == 3)) {
    return Get(args[1], args.size() == 3 ? args[2] : "");
  }
  if (command == "info" && args.size() == 2) return Info(args[1]);
  if (command == "nodes" && args.size() == 1) return Nodes();
  return Usage();
}

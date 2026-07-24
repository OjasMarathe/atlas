// Atlas Phase 0 node skeleton.
//
// Proves the full toolchain end to end: proto codegen (common + storage), a gRPC server
// implementing StorageService.Heartbeat, a gRPC client pinging peers, and multi-node wiring
// under docker-compose. This is the embryo of the Phase 2 heartbeat/failure-detection loop.
//
// Config via env:
//   ATLAS_NODE_ID   node's id                 (default "node-0")
//   ATLAS_LISTEN    listen address host:port  (default "0.0.0.0:50051")
//   ATLAS_PEERS     comma-separated peers      (default "" -> no peers)

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "common.pb.h"
#include "storage.grpc.pb.h"

namespace {

std::string EnvOr(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

std::vector<std::string> Split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, delim)) {
    if (!tok.empty()) out.push_back(tok);
  }
  return out;
}

void Log(const std::string& node, const std::string& msg) {
  std::cout << "[" << node << "] " << msg << std::endl;
}

}  // namespace

// Minimal StorageService: only Heartbeat is implemented for Phase 0; every other RPC returns
// UNIMPLEMENTED by default (the generated base class handles that).
class NodeService final : public atlas::StorageService::Service {
 public:
  explicit NodeService(std::string id) : id_(std::move(id)) {}

  grpc::Status Heartbeat(grpc::ServerContext*, const atlas::HeartbeatRequest* req,
                         atlas::HeartbeatResponse* resp) override {
    Log(id_, "<- heartbeat from " + req->from_node_id());
    resp->mutable_status()->set_code(atlas::Status::OK);
    resp->set_ring_version(0);
    return grpc::Status::OK;
  }

 private:
  std::string id_;
};

int main() {
  const std::string id = EnvOr("ATLAS_NODE_ID", "node-0");
  const std::string listen = EnvOr("ATLAS_LISTEN", "0.0.0.0:50051");
  const std::vector<std::string> peers = Split(EnvOr("ATLAS_PEERS", ""), ',');

  NodeService service(id);
  grpc::EnableDefaultHealthCheckService(true);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "[" << id << "] failed to bind " << listen << std::endl;
    return 1;
  }
  Log(id, "listening on " + listen +
              " | peers: " + (peers.empty() ? "(none)" : EnvOr("ATLAS_PEERS", "")));

  std::atomic<bool> running{true};
  std::thread heartbeats([&] {
    std::this_thread::sleep_for(std::chrono::seconds(2));  // let peers come up
    while (running.load()) {
      for (const auto& peer : peers) {
        auto channel = grpc::CreateChannel(peer, grpc::InsecureChannelCredentials());
        auto stub = atlas::StorageService::NewStub(channel);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        atlas::HeartbeatRequest req;
        req.set_from_node_id(id);
        req.set_ring_version_seen(0);
        atlas::HeartbeatResponse resp;
        const grpc::Status st = stub->Heartbeat(&ctx, req, &resp);
        Log(id, st.ok() ? ("-> peer " + peer + " OK")
                        : ("-> peer " + peer + " UNREACHABLE: " + st.error_message()));
      }
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
  });

  server->Wait();
  running.store(false);
  heartbeats.join();
  return 0;
}

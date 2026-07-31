// Atlas storage + search node.
//
// Serves two roles on one port: StorageService over a local RocksDB chunk store (Phase 1) and
// this node's own SearchService shard (Phase 3, ADR-0006 — a shard indexes the documents whose
// chunks live on its co-located storage node). Also pings its peers, the embryo of the Phase 2
// heartbeat/failure-detection loop.
//
// Config via env:
//   ATLAS_NODE_ID   node's id                 (default "node-0")
//   ATLAS_LISTEN    listen address host:port  (default "0.0.0.0:50051")
//   ATLAS_PEERS     comma-separated peers     (default "" -> no peers)
//   ATLAS_DATA_DIR  chunk store path          (default "atlas-data-<node id>")

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "common.pb.h"
#include "storage.grpc.pb.h"
#include "storage/chunk_store.h"
#include "storage/storage_service.h"

#include "search/search_service.h"

namespace {

// Set by the signal handler, polled by the shutdown-watcher thread. std::atomic<bool> is
// lock-free on every platform we target, so it is safe to touch from a signal handler.
std::atomic<bool> g_stop{false};

void OnSignal(int /*sig*/) { g_stop.store(true); }

std::mutex g_log_mutex;

void Log(const std::string& node, const std::string& msg) {
  // Serialize writes: Log() runs on both the RPC and heartbeat threads, and a bare
  // `std::cout <<` chain from multiple threads can interleave mid-line.
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cout << "[" << node << "] " << msg << std::endl;
}

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

}  // namespace

int main() {
  // Line-buffer stdout so logs appear in real time even when redirected to a file or pipe.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  const std::string id = EnvOr("ATLAS_NODE_ID", "node-0");
  const std::string listen = EnvOr("ATLAS_LISTEN", "0.0.0.0:50051");
  const std::vector<std::string> peers = Split(EnvOr("ATLAS_PEERS", ""), ',');

  const std::string data_dir = EnvOr("ATLAS_DATA_DIR", "atlas-data-" + id);
  atlas::ChunkStore store(data_dir);
  if (!store.ok()) {
    std::cerr << "[" << id << "] failed to open chunk store at " << data_dir << std::endl;
    return 1;
  }
  atlas::StorageServiceImpl service(&store);

  // Every node also serves its own search shard (ADR-0006: a shard indexes the documents whose
  // chunks live on its co-located storage node). Empty until something calls IndexDocument.
  atlas::search::SearchServiceImpl search_service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  builder.RegisterService(&search_service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "[" << id << "] failed to bind " << listen << std::endl;
    return 1;
  }
  Log(id, "listening on " + listen +
              " | peers: " + (peers.empty() ? "(none)" : EnvOr("ATLAS_PEERS", "")));

  // Graceful shutdown: SIGINT/SIGTERM (e.g. `docker compose down`) flip g_stop; a watcher
  // thread then calls server->Shutdown(), which unblocks server->Wait() below so the cleanup
  // path actually runs. We shut down from a normal thread because Shutdown() is not
  // async-signal-safe to call directly from the handler.
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

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

  std::thread watcher([&] {
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Log(id, "shutdown signal received, stopping...");
    server->Shutdown();
  });

  server->Wait();  // returns once the watcher calls Shutdown()
  running.store(false);
  heartbeats.join();
  watcher.join();
  return 0;
}

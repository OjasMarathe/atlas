// Atlas node — runs as either a storage node or the metadata (control-plane) node.
//
//   ATLAS_ROLE=storage  (default)
//     Serves StorageService over a local RocksDB chunk store (Phase 1) *and* this node's own
//     SearchService shard on the same port (Phase 3, ADR-0006 — a shard indexes the documents
//     whose chunks live on its co-located storage node). If ATLAS_METADATA is set, it registers
//     itself into the ring at startup (retrying until the control plane is up).
//
//   ATLAS_ROLE=metadata
//     Serves MetadataService (file map + ring) and runs the maintenance loop: probe every member
//     for liveness, then re-replicate any chunk that has fallen below the replication factor.
//
//   ATLAS_ROLE=coordinator
//     Serves CoordinatorService (Phase 4): discovers search shards from the ring, scatter-gathers
//     each query across all of them, merges their local top-Ks, and caches the result.
//
// Config via env:
//   ATLAS_NODE_ID    node's id                  (default "node-0")
//   ATLAS_LISTEN     bind address host:port     (default "0.0.0.0:50051")
//   ATLAS_ADVERTISE  address peers should dial  (default: ATLAS_LISTEN)
//   ATLAS_DATA_DIR   on-disk state path         (default "atlas-data-<node id>")
//   ATLAS_METADATA   metadata address           (storage: register here; coordinator: discover)
//   ATLAS_HEAL_INTERVAL_MS / ATLAS_FAILURE_THRESHOLD  (metadata role: loop tuning)
//   ATLAS_CACHE_POLICY / ATLAS_CACHE_CAPACITY / ATLAS_SHARD_TIMEOUT_MS / ATLAS_GLOBAL_SCORING
//                                               (coordinator role: tuning)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "cluster/maintenance.h"
#include "coordinator/coordinator.h"
#include "coordinator/coordinator_service.h"
#include "metadata.grpc.pb.h"
#include "metadata/metadata_service.h"
#include "metadata/metadata_store.h"
#include "storage/chunk_store.h"
#include "storage/storage_service.h"

#include "search/search_service.h"

namespace {

// Set by the signal handler, polled by the shutdown-watcher thread. std::atomic<bool> is
// lock-free on every platform we target, so it is safe to touch from a signal handler.
std::atomic<bool> g_stop{false};

void OnSignal(int /*sig*/) { g_stop.store(true); }

std::mutex g_log_mutex;
std::string g_id = "node";

void Log(const std::string& msg) {
  // Serialize writes: Log() runs on the RPC, maintenance and registration threads, and a bare
  // `std::cout <<` chain from multiple threads can interleave mid-line.
  const std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cout << "[" << g_id << "] " << msg << std::endl;
}

std::string EnvOr(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

int EnvInt(const char* key, int fallback) {
  const std::string v = EnvOr(key, "");
  return v.empty() ? fallback : std::atoi(v.c_str());
}

// Blocks until the server is told to shut down, then returns. Shutdown happens on a normal
// thread because grpc::Server::Shutdown() is not async-signal-safe.
void ServeUntilSignalled(grpc::Server* server) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  std::thread watcher([server] {
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Log("shutdown signal received, stopping...");
    server->Shutdown();
  });
  server->Wait();
  watcher.join();
}

// Storage nodes announce themselves to the control plane. Retried because in a compose cluster
// the metadata node may still be starting; the ring stays empty until this succeeds.
void RegisterWithMetadata(const std::string& metadata_address, const std::string& node_id,
                          const std::string& advertise) {
  auto stub = atlas::MetadataService::NewStub(
      grpc::CreateChannel(metadata_address, grpc::InsecureChannelCredentials()));
  while (!g_stop.load()) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    atlas::UpdateMembershipRequest request;
    request.mutable_node()->set_node_id(node_id);
    request.mutable_node()->set_address(advertise);
    request.mutable_node()->add_roles(atlas::STORAGE);
    // The same process serves this node's search shard on the same port, so it advertises both.
    // Phase 4's coordinator discovers shards by filtering the ring on SEARCH — a node that only
    // claimed STORAGE would store documents nobody could ever query.
    request.mutable_node()->add_roles(atlas::SEARCH);
    request.set_change(atlas::UpdateMembershipRequest::JOIN);
    atlas::RingState response;
    if (stub->UpdateMembership(&ctx, request, &response).ok()) {
      Log("joined the ring at " + metadata_address + " as " + advertise + " (ring version " +
          std::to_string(response.version()) + ", " + std::to_string(response.nodes_size()) +
          " nodes)");
      return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

int RunStorage(const std::string& id, const std::string& listen, const std::string& advertise) {
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
  Log("storage node listening on " + listen + " | data " + data_dir);

  std::thread registrar;
  const std::string metadata_address = EnvOr("ATLAS_METADATA", "");
  if (!metadata_address.empty()) {
    registrar = std::thread([&] { RegisterWithMetadata(metadata_address, id, advertise); });
  }

  ServeUntilSignalled(server.get());
  if (registrar.joinable()) registrar.join();
  return 0;
}

int RunMetadata(const std::string& id, const std::string& listen) {
  const std::string data_dir = EnvOr("ATLAS_DATA_DIR", "atlas-data-" + id);
  atlas::MetadataStore store(data_dir);
  if (!store.ok()) {
    std::cerr << "[" << id << "] failed to open metadata store at " << data_dir << std::endl;
    return 1;
  }
  atlas::MetadataServiceImpl service(&store);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "[" << id << "] failed to bind " << listen << std::endl;
    return 1;
  }

  atlas::MaintenanceOptions options;
  options.interval = std::chrono::milliseconds(EnvInt("ATLAS_HEAL_INTERVAL_MS", 2000));
  options.failure_threshold = EnvInt("ATLAS_FAILURE_THRESHOLD", 3);
  atlas::ClusterMaintenance maintenance(
      &store, [&service] { return service.SnapshotRing(); }, [](const std::string& m) { Log(m); },
      options);

  Log("metadata node listening on " + listen + " | data " + data_dir);
  Log("maintenance loop every " + std::to_string(options.interval.count()) + "ms, node declared " +
      "dead after " + std::to_string(options.failure_threshold) + " missed probes");
  maintenance.Start();

  ServeUntilSignalled(server.get());
  maintenance.Stop();
  return 0;
}

int RunCoordinator(const std::string& id, const std::string& listen) {
  const std::string metadata_address = EnvOr("ATLAS_METADATA", "127.0.0.1:50050");

  atlas::coordinator::CoordinatorOptions options;
  options.cache_capacity = static_cast<std::size_t>(EnvInt("ATLAS_CACHE_CAPACITY", 256));
  options.cache_policy = EnvOr("ATLAS_CACHE_POLICY", "lru");
  options.shard_timeout = std::chrono::milliseconds(EnvInt("ATLAS_SHARD_TIMEOUT_MS", 1500));
  options.global_scoring = EnvInt("ATLAS_GLOBAL_SCORING", 1) != 0;

  atlas::coordinator::RingShardDirectory directory(metadata_address);
  atlas::coordinator::QueryCoordinator coordinator([&directory] { return directory.Shards(); },
                                                   options);
  atlas::coordinator::CoordinatorServiceImpl service(&coordinator);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "[" << id << "] failed to bind " << listen << std::endl;
    return 1;
  }

  Log("coordinator listening on " + listen + " | metadata " + metadata_address);
  Log("result cache: " + options.cache_policy + ", capacity " +
      std::to_string(options.cache_capacity) + " | global scoring " +
      (options.global_scoring ? "on (2-round DFS)" : "off (shard-local scores)"));

  ServeUntilSignalled(server.get());
  return 0;
}

}  // namespace

int main() {
  // Line-buffer stdout so logs appear in real time even when redirected to a file or pipe.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  g_id = EnvOr("ATLAS_NODE_ID", "node-0");
  const std::string role = EnvOr("ATLAS_ROLE", "storage");
  const std::string listen = EnvOr("ATLAS_LISTEN", "0.0.0.0:50051");
  const std::string advertise = EnvOr("ATLAS_ADVERTISE", listen);

  if (role == "metadata") return RunMetadata(g_id, listen);
  if (role == "coordinator") return RunCoordinator(g_id, listen);
  if (role != "storage") {
    std::cerr << "unknown ATLAS_ROLE '" << role
              << "' (expected 'storage', 'metadata' or 'coordinator')\n";
    return 2;
  }
  return RunStorage(g_id, listen, advertise);
}

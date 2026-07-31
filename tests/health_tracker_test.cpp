// Failure-detection tests (Phase 2 slice A): HealthTracker's threshold semantics as pure logic,
// then the Prober against real loopback storage nodes — including one that is killed mid-test.

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cluster/health_tracker.h"
#include "cluster/prober.h"
#include "storage/chunk_store.h"
#include "storage/storage_service.h"

namespace {
int g_checks = 0;
int g_fails = 0;
namespace fs = std::filesystem;
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

  // ---- HealthTracker: threshold, revival, forget ----
  {
    HealthTracker tracker(/*failure_threshold=*/3);

    CHECK(tracker.IsAlive("never-probed"));  // unprobed nodes are assumed alive

    tracker.RecordFailure("n1");
    CHECK(tracker.IsAlive("n1"));  // one miss is not death
    tracker.RecordFailure("n1");
    CHECK(tracker.IsAlive("n1"));
    tracker.RecordFailure("n1");
    CHECK(!tracker.IsAlive("n1"));  // threshold reached
    CHECK_EQ(tracker.ConsecutiveFailures("n1"), 3);
    CHECK_EQ(tracker.DeadNodes(), (std::vector<std::string>{"n1"}));

    tracker.RecordSuccess("n1");  // a single success revives immediately
    CHECK(tracker.IsAlive("n1"));
    CHECK_EQ(tracker.ConsecutiveFailures("n1"), 0);
    CHECK(tracker.DeadNodes().empty());

    // Failures must be *consecutive*: an intervening success resets the count.
    tracker.RecordFailure("n2");
    tracker.RecordFailure("n2");
    tracker.RecordSuccess("n2");
    tracker.RecordFailure("n2");
    CHECK(tracker.IsAlive("n2"));

    tracker.RecordFailure("n3");
    tracker.RecordFailure("n3");
    tracker.RecordFailure("n3");
    CHECK(!tracker.IsAlive("n3"));
    tracker.Forget("n3");  // node left the cluster
    CHECK(tracker.IsAlive("n3"));
    CHECK(tracker.DeadNodes().empty());
  }

  // ---- Prober against real storage nodes ----
  struct Node {
    std::string id;
    std::string path;
    std::unique_ptr<ChunkStore> store;
    std::unique_ptr<StorageServiceImpl> service;
    std::unique_ptr<grpc::Server> server;
    std::string address;
  };

  std::vector<std::unique_ptr<Node>> nodes;
  for (int i = 1; i <= 2; ++i) {
    auto node = std::make_unique<Node>();
    node->id = "storage-" + std::to_string(i);
    node->path = (fs::temp_directory_path() / ("atlas_health_" + node->id)).string();
    fs::remove_all(node->path);
    node->store = std::make_unique<ChunkStore>(node->path);
    node->service = std::make_unique<StorageServiceImpl>(node->store.get());
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(node->service.get());
    node->server = builder.BuildAndStart();
    node->address = "127.0.0.1:" + std::to_string(port);
    nodes.push_back(std::move(node));
  }

  const std::vector<ProbeTarget> targets{{nodes[0]->id, nodes[0]->address},
                                         {nodes[1]->id, nodes[1]->address}};

  HealthTracker tracker(/*failure_threshold=*/2);
  Prober prober(&tracker, std::chrono::milliseconds(500));

  // Both up: every probe answers, nothing is dead.
  CHECK_EQ(prober.ProbeOnce(targets), 2);
  CHECK(tracker.IsAlive(nodes[0]->id));
  CHECK(tracker.IsAlive(nodes[1]->id));
  CHECK(tracker.DeadNodes().empty());

  // Kill node 2. The first missed probe must NOT evict it (below threshold)...
  nodes[1]->server->Shutdown();
  nodes[1]->server.reset();
  CHECK_EQ(prober.ProbeOnce(targets), 1);
  CHECK(tracker.IsAlive(nodes[1]->id));

  // ...the second one does.
  CHECK_EQ(prober.ProbeOnce(targets), 1);
  CHECK(!tracker.IsAlive(nodes[1]->id));
  CHECK_EQ(tracker.DeadNodes(), (std::vector<std::string>{nodes[1]->id}));

  // The survivor is unaffected by its peer's death.
  CHECK(tracker.IsAlive(nodes[0]->id));

  for (auto& node : nodes) {
    if (node->server) node->server->Shutdown();
  }
  for (const auto& node : nodes) fs::remove_all(node->path);

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

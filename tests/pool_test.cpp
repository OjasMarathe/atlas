// ThreadPool and ConnectionPool (Phase 4).
//
// The thread pool is tested as pure logic; the connection pool needs real endpoints, so it runs
// against loopback SearchService servers and checks the property that matters — that it reuses
// connections instead of opening one per call, and that it never exceeds its per-endpoint cap.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/pool/connection_pool.h"
#include "common/pool/thread_pool.h"
#include "search/search_service.h"

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
  using atlas::ConnectionPool;
  using atlas::ThreadPool;

  // ---- ThreadPool: results come back, and every task runs exactly once ----
  {
    ThreadPool pool(4);
    CHECK_EQ(pool.size(), 4u);

    std::vector<std::future<int>> futures;
    futures.reserve(100);
    for (int i = 0; i < 100; ++i) {
      futures.push_back(pool.Submit([](int value) { return value * 2; }, i));
    }
    int sum = 0;
    for (auto& future : futures) sum += future.get();
    CHECK_EQ(sum, 9900);  // 2 * (0 + 1 + ... + 99)
  }

  // ---- Work really is spread across threads, not serialized onto one ----
  {
    ThreadPool pool(4);
    std::mutex seen_mutex;
    std::set<std::thread::id> seen;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 32; ++i) {
      futures.push_back(pool.Submit([&seen, &seen_mutex] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const std::lock_guard<std::mutex> lock(seen_mutex);
        seen.insert(std::this_thread::get_id());
      }));
    }
    for (auto& future : futures) future.get();
    CHECK(seen.size() > 1);
    CHECK(seen.size() <= 4);
  }

  // ---- An exception on a worker must surface at the future, not call std::terminate ----
  {
    ThreadPool pool(2);
    auto future = pool.Submit([] { throw std::runtime_error("boom"); });
    bool threw = false;
    try {
      future.get();
    } catch (const std::runtime_error&) {
      threw = true;
    }
    CHECK(threw);

    // ...and the pool still works afterwards.
    CHECK_EQ(pool.Submit([] { return 7; }).get(), 7);
  }

  // ---- WaitIdle drains, and queued work is never dropped on shutdown ----
  {
    ThreadPool pool(3);
    std::atomic<int> completed{0};
    for (int i = 0; i < 50; ++i) {
      pool.Submit([&completed] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++completed;
      });
    }
    pool.WaitIdle();
    CHECK_EQ(completed.load(), 50);
    CHECK_EQ(pool.pending(), 0u);

    pool.Shutdown();
    pool.Shutdown();  // idempotent
  }

  // ---- ConnectionPool against real loopback shards ----
  struct Shard {
    std::unique_ptr<atlas::search::SearchServiceImpl> service;
    std::unique_ptr<grpc::Server> server;
    std::string address;
  };

  std::vector<std::unique_ptr<Shard>> shards;
  for (int i = 0; i < 2; ++i) {
    auto shard = std::make_unique<Shard>();
    shard->service = std::make_unique<atlas::search::SearchServiceImpl>();
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(shard->service.get());
    shard->server = builder.BuildAndStart();
    shard->address = "127.0.0.1:" + std::to_string(port);
    shards.push_back(std::move(shard));
  }

  {
    ConnectionPool<atlas::SearchService> pool(/*max_per_endpoint=*/2);

    // Sequential acquisitions of the same endpoint must reuse one connection: the lease is
    // returned at the end of each scope, so the pool never needs a second.
    for (int i = 0; i < 10; ++i) {
      auto lease = pool.Acquire(shards[0]->address);
      CHECK(lease.get() != nullptr);
      grpc::ClientContext context;
      atlas::StatsRequest request;
      atlas::ShardStats response;
      CHECK(lease->Stats(&context, request, &response).ok());
    }
    const atlas::ConnectionPoolStats stats = pool.stats();
    CHECK_EQ(stats.created, 1u);
    CHECK_EQ(stats.reused, 9u);
    CHECK_EQ(stats.live, 1u);
  }

  {
    // Held leases force new connections, up to the cap — and the cap is honoured.
    ConnectionPool<atlas::SearchService> pool(/*max_per_endpoint=*/2);
    auto first = pool.Acquire(shards[0]->address);
    auto second = pool.Acquire(shards[0]->address);
    CHECK_EQ(pool.stats().created, 2u);
    CHECK_EQ(pool.stats().live, 2u);

    // A third acquisition must block until one is returned. Prove it by releasing from another
    // thread and observing that Acquire only returns afterwards.
    std::atomic<bool> released{false};
    std::thread releaser([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      released.store(true);
      auto discard = std::move(first);  // returns to the pool at end of scope
    });
    auto third = pool.Acquire(shards[0]->address);
    CHECK(released.load());  // we cannot have got here before the release
    releaser.join();
    CHECK_EQ(pool.stats().created, 2u);  // reused the returned one rather than exceeding the cap
    CHECK(pool.stats().waited >= 1u);
  }

  {
    // Distinct endpoints are pooled independently.
    ConnectionPool<atlas::SearchService> pool(/*max_per_endpoint=*/1);
    auto a = pool.Acquire(shards[0]->address);
    auto b = pool.Acquire(shards[1]->address);
    CHECK(a.get() != b.get());
    CHECK_EQ(pool.stats().endpoints, 2u);
    CHECK_EQ(pool.stats().created, 2u);
  }

  for (auto& shard : shards) shard->server->Shutdown();

  if (g_fails == 0) {
    std::printf("ALL %d CHECKS PASSED\n", g_checks);
    return 0;
  }
  std::printf("%d / %d CHECKS FAILED\n", g_fails, g_checks);
  return 1;
}

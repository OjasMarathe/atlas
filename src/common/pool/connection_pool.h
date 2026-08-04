#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace atlas {

struct ConnectionPoolStats {
  std::uint64_t created = 0;  // channels actually opened
  std::uint64_t reused = 0;   // acquisitions served from the idle set
  std::uint64_t waited = 0;   // acquisitions that had to block for a free connection
  std::size_t endpoints = 0;  // distinct addresses seen
  std::size_t live = 0;       // connections currently held by the pool (idle + leased)
};

// Bounded pool of gRPC channels+stubs, keyed by endpoint address.
//
// An honest caveat first, because it changes what this class is *for*: a gRPC channel is already
// thread-safe and multiplexes many concurrent RPCs over one HTTP/2 connection, so unlike a
// database pool this one is not required for correctness — you can share a single channel across
// threads. Its jobs are narrower and still real:
//
//   1. Amortize setup. Creating a channel costs name resolution and a connection handshake;
//      doing that per query, per shard, is pure latency added to every fan-out.
//   2. Bound the fan-out. HTTP/2 caps concurrent streams per connection (commonly 100). Past
//      that, requests queue *inside* the channel and look like server slowness. A handful of
//      channels per endpoint raises the ceiling; unbounded channels just move the resource
//      exhaustion somewhere harder to see.
//   3. Give callers lease semantics, so "how many in-flight calls may this process have to one
//      shard" is a number we set rather than a number we discover in production.
//
// See docs/concepts/connection-pool.md.
template <typename Service>
class ConnectionPool {
 public:
  using Stub = typename Service::Stub;

  explicit ConnectionPool(std::size_t max_per_endpoint = 4)
      : max_per_endpoint_(max_per_endpoint == 0 ? 1 : max_per_endpoint) {}

  ConnectionPool(const ConnectionPool&) = delete;
  ConnectionPool& operator=(const ConnectionPool&) = delete;

  // A borrowed connection. Returns itself to the pool on destruction, including when the caller
  // unwinds on an exception — the whole reason this is RAII and not a get/put pair.
  class Lease {
   public:
    Lease() = default;
    Lease(ConnectionPool* pool, std::string address, std::unique_ptr<Stub> stub)
        : pool_(pool), address_(std::move(address)), stub_(std::move(stub)) {}

    ~Lease() {
      if (pool_ != nullptr && stub_ != nullptr) pool_->Release(address_, std::move(stub_));
    }

    Lease(Lease&& other) noexcept
        : pool_(other.pool_), address_(std::move(other.address_)), stub_(std::move(other.stub_)) {
      other.pool_ = nullptr;
    }

    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        if (pool_ != nullptr && stub_ != nullptr) pool_->Release(address_, std::move(stub_));
        pool_ = other.pool_;
        address_ = std::move(other.address_);
        stub_ = std::move(other.stub_);
        other.pool_ = nullptr;
      }
      return *this;
    }

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    Stub* get() const { return stub_.get(); }
    Stub* operator->() const { return stub_.get(); }
    explicit operator bool() const { return stub_ != nullptr; }

   private:
    ConnectionPool* pool_ = nullptr;
    std::string address_;
    std::unique_ptr<Stub> stub_;
  };

  // Borrows a connection to `address`, creating one if the endpoint is under its limit and
  // blocking until one is returned if it is not.
  Lease Acquire(const std::string& address) {
    std::unique_lock<std::mutex> lock(mutex_);
    Endpoint& endpoint = endpoints_[address];

    while (endpoint.idle.empty() && endpoint.live >= max_per_endpoint_) {
      ++waited_;
      endpoint.available.wait(lock);
    }

    if (!endpoint.idle.empty()) {
      std::unique_ptr<Stub> stub = std::move(endpoint.idle.back());
      endpoint.idle.pop_back();
      ++reused_;
      return Lease(this, address, std::move(stub));
    }

    ++endpoint.live;
    ++created_;
    lock.unlock();  // channel creation can resolve names; don't hold the pool lock for it
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    return Lease(this, address, Service::NewStub(channel));
  }

  ConnectionPoolStats stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    ConnectionPoolStats out{created_, reused_, waited_, endpoints_.size(), 0};
    for (const auto& [address, endpoint] : endpoints_) out.live += endpoint.live;
    return out;
  }

 private:
  struct Endpoint {
    std::vector<std::unique_ptr<Stub>> idle;
    std::size_t live = 0;  // idle + currently leased
    std::condition_variable available;
  };

  void Release(const std::string& address, std::unique_ptr<Stub> stub) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Endpoint& endpoint = endpoints_[address];
    endpoint.idle.push_back(std::move(stub));
    endpoint.available.notify_one();
  }

  mutable std::mutex mutex_;
  // node_handle stability matters: Endpoint holds a condition_variable, which is neither
  // copyable nor movable, and callers wait on a reference to it across the unordered_map's
  // rehashes. unordered_map never invalidates references to elements, so this is safe.
  std::unordered_map<std::string, Endpoint> endpoints_;
  std::size_t max_per_endpoint_;
  std::uint64_t created_ = 0;
  std::uint64_t reused_ = 0;
  std::uint64_t waited_ = 0;
};

}  // namespace atlas

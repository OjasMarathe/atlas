#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/cache/cache.h"

namespace atlas::cache {

// Least Recently Used: evict the entry untouched for the longest time.
//
// Structure: a std::list holding (key, value) in recency order — front = most recent — plus a
// hash map key -> iterator into that list. The map gives O(1) lookup; splice() moves a node to
// the front in O(1) *without* invalidating the iterator, which is exactly why the map can store
// iterators at all. Evicting is then list.back(): O(1), no scan.
//
// Thread-safe. Note that Get() *mutates* (it promotes the entry), so even reads take the
// exclusive lock — a shared_mutex would buy nothing here.
template <typename Key, typename Value>
class LruCache final : public Cache<Key, Value> {
 public:
  explicit LruCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  std::optional<Value> Get(const Key& key) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end()) {
      ++misses_;
      return std::nullopt;
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, it->second);  // promote to most-recent
    return it->second->second;
  }

  void Put(const Key& key, Value value) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(key);
    if (it != index_.end()) {  // update in place, and it counts as a use
      it->second->second = std::move(value);
      entries_.splice(entries_.begin(), entries_, it->second);
      return;
    }
    if (entries_.size() >= capacity_) {
      index_.erase(entries_.back().first);
      entries_.pop_back();
      ++evictions_;
    }
    entries_.emplace_front(key, std::move(value));
    index_[key] = entries_.begin();
  }

  void Clear() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    index_.clear();
  }

  Stats stats() const override {
    const std::lock_guard<std::mutex> lock(mutex_);
    return Stats{hits_, misses_, evictions_, entries_.size(), capacity_};
  }

  std::string policy() const override { return "lru"; }

 private:
  using Entry = std::pair<Key, Value>;
  using EntryList = std::list<Entry>;

  mutable std::mutex mutex_;
  std::size_t capacity_;
  EntryList entries_;                                            // front = most recently used
  std::unordered_map<Key, typename EntryList::iterator> index_;  // key -> its node
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
  std::uint64_t evictions_ = 0;
};

}  // namespace atlas::cache

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/cache/cache.h"

namespace atlas::cache {

// Least Frequently Used, in O(1) — including eviction.
//
// The naive LFU keeps a count per entry and scans for the minimum on eviction: O(n) per miss,
// which defeats the point. This is the bucket construction: entries are grouped into a list per
// frequency, and `min_frequency_` tracks the lowest non-empty bucket, so the victim is always
// `buckets_[min_frequency_].back()`.
//
// Why `min_frequency_` can only ever move by +1 or reset to 1 — the property that makes it O(1):
// a Get promotes one entry from f to f+1, so the minimum can only rise if that entry was the
// last one at f, and it rises to exactly f+1. Any insertion resets it to 1.
//
// Within a bucket, order is recency (front = most recent), so ties at equal frequency break LRU.
// Without that, a cache full of equally-frequent entries would evict arbitrarily.
template <typename Key, typename Value>
class LfuCache final : public Cache<Key, Value> {
 public:
  explicit LfuCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  std::optional<Value> Get(const Key& key) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      ++misses_;
      return std::nullopt;
    }
    ++hits_;
    Touch(it);
    return it->second.value;
  }

  void Put(const Key& key, Value value) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
      it->second.value = std::move(value);
      Touch(it);
      return;
    }
    if (entries_.size() >= capacity_) Evict();

    auto& bucket = buckets_[1];
    bucket.push_front(key);
    entries_.emplace(key, Entry{std::move(value), 1, bucket.begin()});
    min_frequency_ = 1;  // the newcomer is now the least frequently used
  }

  void Clear() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    buckets_.clear();
    min_frequency_ = 0;
  }

  Stats stats() const override {
    const std::lock_guard<std::mutex> lock(mutex_);
    return Stats{hits_, misses_, evictions_, entries_.size(), capacity_};
  }

  std::string policy() const override { return "lfu"; }

 private:
  using KeyList = std::list<Key>;

  struct Entry {
    Value value;
    std::uint64_t frequency;
    typename KeyList::iterator position;  // where this key sits in buckets_[frequency]
  };

  using EntryMap = std::unordered_map<Key, Entry>;

  // Move an entry from its bucket to the next one up. Splice keeps it O(1) and leaves the
  // stored iterator valid.
  void Touch(typename EntryMap::iterator it) {
    Entry& entry = it->second;
    const std::uint64_t from = entry.frequency;
    KeyList& source = buckets_[from];
    KeyList& target = buckets_[from + 1];

    target.splice(target.begin(), source, entry.position);
    entry.position = target.begin();
    entry.frequency = from + 1;

    // If that emptied the minimum bucket, the new minimum is exactly one higher.
    if (source.empty()) {
      buckets_.erase(from);
      if (min_frequency_ == from) min_frequency_ = from + 1;
    }
  }

  void Evict() {
    const auto bucket = buckets_.find(min_frequency_);
    if (bucket == buckets_.end() || bucket->second.empty()) return;
    entries_.erase(bucket->second.back());  // least frequent, and least recent among those
    bucket->second.pop_back();
    if (bucket->second.empty()) buckets_.erase(bucket);
    ++evictions_;
  }

  mutable std::mutex mutex_;
  std::size_t capacity_;
  EntryMap entries_;
  std::unordered_map<std::uint64_t, KeyList> buckets_;  // frequency -> keys, front = most recent
  std::uint64_t min_frequency_ = 0;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
  std::uint64_t evictions_ = 0;
};

}  // namespace atlas::cache

#include "metadata/metadata_store.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <utility>

namespace atlas {
namespace {

// Per-version key, zero-padded so lexicographic order == numeric order for prefix scans.
std::string VerKey(const std::string& file_id, uint64_t version) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%020llu", static_cast<unsigned long long>(version));
  return "f/" + file_id + "/" + buf;
}
std::string LatestKey(const std::string& file_id) { return "L/" + file_id; }
std::string VerPrefix(const std::string& file_id) { return "f/" + file_id + "/"; }

// Chunk location index: chunk_id -> the nodes currently holding it (a serialized ChunkPlacement).
constexpr const char* kLocPrefix = "c/";
std::string LocKey(const std::string& chunk_id) { return kLocPrefix + chunk_id; }

void SetNow(google::protobuf::Timestamp* ts) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now);
  ts->set_seconds(secs.count());
  ts->set_nanos(static_cast<int32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - secs).count()));
}

}  // namespace

MetadataStore::MetadataStore(const std::string& path) {
  rocksdb::Options options;
  options.create_if_missing = true;
  const rocksdb::Status s = rocksdb::DB::Open(options, path, &db_);
  if (!s.ok()) db_.reset();
}

MetadataStore::~MetadataStore() = default;

uint64_t MetadataStore::LatestVersion(const std::string& file_id) const {
  std::string v;
  if (!db_->Get(rocksdb::ReadOptions(), LatestKey(file_id), &v).ok()) return 0;
  return std::strtoull(v.c_str(), nullptr, 10);
}

FileMetadata MetadataStore::RegisterFile(FileMetadata meta) {
  if (!db_) return meta;
  // Exclusive: both the version allocation below and the location merge further down are
  // read-modify-write. Two concurrent registrations of the same file would otherwise pick the
  // same `next` and one would silently overwrite the other's version.
  const std::unique_lock<std::shared_mutex> lock(mutex_);
  const uint64_t next = LatestVersion(meta.file_id()) + 1;
  meta.set_version(next);
  if (!meta.has_created_at()) SetNow(meta.mutable_created_at());
  SetNow(meta.mutable_modified_at());

  std::string bytes;
  meta.SerializeToString(&bytes);

  // Atomic commit. This is the system's commit point (ADR-0004: a file exists once metadata
  // records it), so the version blob and the "latest" pointer must land together — as two
  // separate Puts, a crash between them would durably store a version that LatestVersion never
  // returns, leaving the just-committed file invisible while GetFile(version=0) served stale data.
  rocksdb::WriteBatch batch;
  batch.Put(VerKey(meta.file_id(), next), bytes);
  batch.Put(LatestKey(meta.file_id()), std::to_string(next));

  // Fold this version's holders into the location index, in the same atomic batch. Merged rather
  // than overwritten: the same content-addressed chunk may already be held for another file or
  // version, and those holders are still valid.
  for (const ChunkPlacement& placement : meta.chunks()) {
    const std::string& chunk_id = placement.chunk().chunk_id();
    ChunkPlacement merged = LoadLocations(chunk_id);
    merged.mutable_chunk()->CopyFrom(placement.chunk());
    std::unordered_set<std::string> present(merged.node_ids().begin(), merged.node_ids().end());
    for (const std::string& node_id : placement.node_ids()) {
      if (present.insert(node_id).second) merged.add_node_ids(node_id);
    }
    std::string loc_bytes;
    merged.SerializeToString(&loc_bytes);
    batch.Put(LocKey(chunk_id), loc_bytes);
  }

  rocksdb::WriteOptions wo;
  wo.sync = true;
  db_->Write(wo, &batch);
  return meta;
}

ChunkPlacement MetadataStore::LoadLocations(const std::string& chunk_id) const {
  ChunkPlacement placement;
  if (!db_) return placement;
  std::string bytes;
  if (db_->Get(rocksdb::ReadOptions(), LocKey(chunk_id), &bytes).ok()) {
    placement.ParseFromString(bytes);
  }
  return placement;
}

void MetadataStore::FillLiveLocations(FileMetadata* meta) const {
  // A version blob records where the chunks were at write time; the index knows where they are
  // *now* (after self-healing). Readers must always get the current answer.
  for (ChunkPlacement& placement : *meta->mutable_chunks()) {
    const ChunkPlacement live = LoadLocations(placement.chunk().chunk_id());
    placement.clear_node_ids();
    for (const std::string& node_id : live.node_ids()) placement.add_node_ids(node_id);
  }
}

void MetadataStore::AddChunkLocation(const std::string& chunk_id, const std::string& node_id) {
  if (!db_) return;
  const std::unique_lock<std::shared_mutex> lock(mutex_);
  ChunkPlacement placement = LoadLocations(chunk_id);
  for (const std::string& existing : placement.node_ids()) {
    if (existing == node_id) return;  // already recorded
  }
  placement.mutable_chunk()->set_chunk_id(chunk_id);
  placement.add_node_ids(node_id);
  std::string bytes;
  placement.SerializeToString(&bytes);
  rocksdb::WriteOptions wo;
  wo.sync = true;
  db_->Put(wo, LocKey(chunk_id), bytes);
}

void MetadataStore::RemoveChunkLocation(const std::string& chunk_id, const std::string& node_id) {
  if (!db_) return;
  const std::unique_lock<std::shared_mutex> lock(mutex_);
  const ChunkPlacement current = LoadLocations(chunk_id);
  ChunkPlacement updated;
  updated.mutable_chunk()->CopyFrom(current.chunk());
  updated.mutable_chunk()->set_chunk_id(chunk_id);
  for (const std::string& existing : current.node_ids()) {
    if (existing != node_id) updated.add_node_ids(existing);
  }
  std::string bytes;
  updated.SerializeToString(&bytes);
  rocksdb::WriteOptions wo;
  wo.sync = true;
  db_->Put(wo, LocKey(chunk_id), bytes);
}

std::vector<std::string> MetadataStore::ChunkLocations(const std::string& chunk_id) const {
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  const ChunkPlacement placement = LoadLocations(chunk_id);
  return {placement.node_ids().begin(), placement.node_ids().end()};
}

std::vector<std::string> MetadataStore::AllChunkIds() const {
  std::vector<std::string> ids;
  if (!db_) return ids;
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  const std::string prefix = kLocPrefix;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
  for (it->Seek(prefix); it->Valid(); it->Next()) {
    const std::string key = it->key().ToString();
    if (key.compare(0, prefix.size(), prefix) != 0) break;
    ids.push_back(key.substr(prefix.size()));
  }
  return ids;
}

bool MetadataStore::GetFile(const std::string& file_id, uint64_t version, FileMetadata* out) const {
  if (!db_) return false;
  // Shared, but held across the whole read: resolving "latest", loading that blob and then
  // overlaying live locations must see one consistent snapshot, or a concurrent RegisterFile can
  // make GetFile serve a version blob alongside a *different* version's holders.
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  if (version == 0) {
    version = LatestVersion(file_id);
    if (version == 0) return false;
  }
  std::string bytes;
  if (!db_->Get(rocksdb::ReadOptions(), VerKey(file_id, version), &bytes).ok()) return false;
  if (!out->ParseFromString(bytes)) return false;
  FillLiveLocations(out);  // serve where the chunks are now, not where they were written
  return true;
}

std::vector<FileMetadata> MetadataStore::ListVersions(const std::string& file_id) const {
  std::vector<FileMetadata> out;
  if (!db_) return out;
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  const std::string prefix = VerPrefix(file_id);
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
  for (it->Seek(prefix); it->Valid(); it->Next()) {
    const std::string key = it->key().ToString();
    if (key.compare(0, prefix.size(), prefix) != 0) break;  // past this file's versions
    FileMetadata m;
    if (m.ParseFromString(it->value().ToString())) out.push_back(std::move(m));
  }
  return out;
}

}  // namespace atlas

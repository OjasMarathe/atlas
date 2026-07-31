#include "metadata/metadata_store.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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
  rocksdb::WriteOptions wo;
  wo.sync = true;
  db_->Write(wo, &batch);
  return meta;
}

bool MetadataStore::GetFile(const std::string& file_id, uint64_t version, FileMetadata* out) const {
  if (!db_) return false;
  if (version == 0) {
    version = LatestVersion(file_id);
    if (version == 0) return false;
  }
  std::string bytes;
  if (!db_->Get(rocksdb::ReadOptions(), VerKey(file_id, version), &bytes).ok()) return false;
  return out->ParseFromString(bytes);
}

std::vector<FileMetadata> MetadataStore::ListVersions(const std::string& file_id) const {
  std::vector<FileMetadata> out;
  if (!db_) return out;
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

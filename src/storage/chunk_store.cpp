#include "storage/chunk_store.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <utility>

#include "common/sha256.h"

namespace atlas {

ChunkStore::ChunkStore(const std::string& path) {
  rocksdb::Options options;
  options.create_if_missing = true;
  // RocksDB 11.x's DB::Open takes a std::unique_ptr<DB>* (not DB**), so we hand it db_ directly.
  const rocksdb::Status s = rocksdb::DB::Open(options, path, &db_);
  if (!s.ok()) db_.reset();
}

ChunkStore::~ChunkStore() = default;  // defined here where rocksdb::DB is complete

StoreStatus ChunkStore::Put(const std::string& chunk_id, const std::string& data) {
  if (!db_) return StoreStatus::kError;
  // Content-address integrity: refuse to store bytes that don't match their claimed id.
  if (Sha256Hex(data) != chunk_id) return StoreStatus::kChecksumMismatch;
  rocksdb::WriteOptions wo;
  wo.sync = true;  // WAL fsync before returning — the durability ADR-0004's W=2 ack relies on
  return db_->Put(wo, chunk_id, data).ok() ? StoreStatus::kOk : StoreStatus::kError;
}

StoreStatus ChunkStore::Get(const std::string& chunk_id, std::string* out) const {
  if (!db_) return StoreStatus::kError;
  std::string value;
  const rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), chunk_id, &value);
  if (s.IsNotFound()) return StoreStatus::kNotFound;
  if (!s.ok()) return StoreStatus::kError;
  if (Sha256Hex(value) != chunk_id) return StoreStatus::kChecksumMismatch;  // silent corruption
  *out = std::move(value);
  return StoreStatus::kOk;
}

StoreStatus ChunkStore::Delete(const std::string& chunk_id) {
  if (!db_) return StoreStatus::kError;
  rocksdb::WriteOptions wo;
  wo.sync = true;
  return db_->Delete(wo, chunk_id).ok() ? StoreStatus::kOk : StoreStatus::kError;
}

bool ChunkStore::Exists(const std::string& chunk_id) const {
  if (!db_) return false;
  std::string value;
  return db_->Get(rocksdb::ReadOptions(), chunk_id, &value).ok();
}

}  // namespace atlas

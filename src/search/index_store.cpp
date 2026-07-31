#include "search/index_store.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <vector>

#include "search/posting_codec.h"

namespace atlas::search {
namespace {

constexpr const char* kPostingsColumnFamily = "postings";
constexpr const char* kMetaKey = "index_metadata";

}  // namespace

IndexStore::IndexStore(const std::string& path) {
  rocksdb::Options options;
  options.create_if_missing = true;
  options.create_missing_column_families = true;

  std::vector<rocksdb::ColumnFamilyDescriptor> families;
  families.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions());
  families.emplace_back(kPostingsColumnFamily, rocksdb::ColumnFamilyOptions());

  std::vector<rocksdb::ColumnFamilyHandle*> handles;
  const rocksdb::Status status = rocksdb::DB::Open(options, path, families, &handles, &db_);
  if (!status.ok()) {
    db_.reset();
    return;
  }
  meta_ = handles[0];  // the default family holds the document table + vocabulary
  postings_ = handles[1];
}

IndexStore::~IndexStore() {
  // RocksDB requires every column-family handle to be released before the DB is closed —
  // otherwise ~ColumnFamilySet trips its `last_ref` assertion. db_ is destroyed after this body
  // runs, so releasing here is the correct order.
  if (db_) {
    if (meta_ != nullptr) db_->DestroyColumnFamilyHandle(meta_);
    if (postings_ != nullptr) db_->DestroyColumnFamilyHandle(postings_);
  }
}

bool IndexStore::Save(const InvertedIndex& index) {
  if (!db_) return false;

  rocksdb::WriteBatch batch;
  batch.Put(meta_, kMetaKey, index.SerializeMetadata());
  index.ForEachPostingList([&](const std::string& term, const PostingList& postings) {
    batch.Put(postings_, term, EncodePostingList(postings));
  });

  // One atomic batch: the document table and the posting lists describe each other, so a
  // partially applied save would be a corrupt index rather than an older one.
  rocksdb::WriteOptions write_options;
  write_options.sync = true;
  return db_->Write(write_options, &batch).ok();
}

bool IndexStore::Load(InvertedIndex* index) const {
  if (!db_) return false;

  std::string metadata;
  if (!db_->Get(rocksdb::ReadOptions(), meta_, kMetaKey, &metadata).ok()) return false;

  InvertedIndex loaded;
  if (!loaded.LoadMetadata(metadata)) return false;

  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), postings_));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    PostingList postings;
    if (!DecodePostingList(it->value().ToString(), &postings)) return false;
    loaded.SetPostingList(it->key().ToString(), std::move(postings));
  }
  if (!it->status().ok()) return false;

  *index = std::move(loaded);  // commit only after everything decoded
  return true;
}

}  // namespace atlas::search

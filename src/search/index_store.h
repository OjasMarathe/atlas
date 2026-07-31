#pragma once

#include <memory>
#include <string>

#include "search/inverted_index.h"

namespace rocksdb {
class ColumnFamilyHandle;
class DB;
}  // namespace rocksdb

namespace atlas::search {

// Persists an InvertedIndex to RocksDB — the storage half of
// docs/architecture/adr/0007-inverted-index-format-compression.md.
//
// Two column families, as the ADR specifies:
//   "postings" : term -> delta+varint encoded posting list (one key per term, so a future
//                incremental flush can rewrite a single term without touching the rest)
//   "meta"     : the document table + vocabulary, under one key
//
// Deviation from ADR-0007 worth naming: the ADR proposed keying postings by an integer term_id
// with a separate term dictionary, to avoid repeating term strings. We key by the term itself.
// It removes an id allocator and a second lookup on the query path, and RocksDB already
// prefix-compresses keys within a block, so the saving the term_id would buy is small at our
// scale. Revisit if the term dictionary ever dominates the on-disk size.
class IndexStore {
 public:
  explicit IndexStore(const std::string& path);
  ~IndexStore();

  IndexStore(const IndexStore&) = delete;
  IndexStore& operator=(const IndexStore&) = delete;

  bool ok() const { return db_ != nullptr; }

  // Writes the whole index. Metadata and postings go in one atomic batch, so a crash mid-save
  // leaves the previous snapshot intact rather than a document table that disagrees with the
  // posting lists.
  bool Save(const InvertedIndex& index);

  // Replaces `*index` with the persisted snapshot. Returns false if nothing is stored or the
  // data is corrupt, leaving `*index` untouched.
  bool Load(InvertedIndex* index) const;

 private:
  std::unique_ptr<rocksdb::DB> db_;
  rocksdb::ColumnFamilyHandle* postings_ = nullptr;  // owned by db_
  rocksdb::ColumnFamilyHandle* meta_ = nullptr;      // owned by db_
};

}  // namespace atlas::search

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "search/types.h"

namespace atlas::search {

// Transparent hashing so term lookups can take a string_view without allocating a std::string
// on every query-time probe.
struct TermHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view term) const {
    return std::hash<std::string_view>{}(term);
  }
};

// One document's entry in a term's posting list.
struct Posting {
  DocId doc_id;
  std::uint32_t term_frequency;          // f(t,D) for BM25
  std::vector<std::uint32_t> positions;  // token offsets in the pre-filter stream (phrase search)
};

// Posting lists are kept sorted by ascending doc_id — required by the set operations in
// postings.h, and by the delta encoding ADR-0007 specifies for the on-disk format.
using PostingList = std::vector<Posting>;

// A shard-local inverted index: term -> the documents containing it.
//
// In-memory for M1. Persisting it to RocksDB with delta+varint compressed posting lists is
// ADR-0007's decision and lands with Phase 3b.
class InvertedIndex {
 public:
  // Analyzes `text` and indexes it. Returns the assigned DocId. Re-indexing a file_id that is
  // already present replaces nothing — see the incremental-indexing gap noted in ADR-0007.
  DocId AddDocument(std::string file_id, std::string_view text);

  // Null when the term is absent from this shard.
  const PostingList* Lookup(std::string_view term) const;

  // n(t): how many documents contain the term. 0 if absent.
  std::size_t DocumentFrequency(std::string_view term) const;

  std::size_t DocumentCount() const { return docs_.size(); }
  std::size_t UniqueTerms() const { return postings_.size(); }

  // |D| in tokens (post stop-word removal), and the corpus average used by BM25's length
  // normalization. AverageDocumentLength() is 0 for an empty index.
  std::uint32_t DocumentLength(DocId doc_id) const;
  double AverageDocumentLength() const;

  const std::string& FileId(DocId doc_id) const;

  // All doc ids in ascending order — the universe a NOT clause complements.
  std::vector<DocId> AllDocuments() const;

 private:
  struct DocumentMeta {
    std::string file_id;
    std::uint32_t length;
  };

  std::unordered_map<std::string, PostingList, TermHash, std::equal_to<>> postings_;
  std::vector<DocumentMeta> docs_;
  std::uint64_t total_length_ = 0;
};

}  // namespace atlas::search

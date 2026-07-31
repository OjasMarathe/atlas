#pragma once

#include <cstdint>
#include <functional>
#include <map>
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
// Updates are incremental: re-indexing a file_id supersedes its previous document, and deletes
// tombstone it. Postings for superseded documents linger until Compact() — the same
// deleted-docs-bitmap + merge approach Lucene uses, because posting lists are append-optimized
// and (once delta-encoded, ADR-0007) order-dependent. See concepts/incremental-indexing.md.
class InvertedIndex {
 public:
  // Indexes `text` under `file_id`, superseding any previous document with that id. `fields`
  // are exact-match attributes for filtering (author/type/lang/...); values are matched
  // verbatim, not analyzed. Returns the new DocId.
  DocId IndexDocument(std::string file_id, std::string_view text,
                      const std::map<std::string, std::string>& fields = {});

  // Tombstones the document. Returns false if the file_id isn't indexed.
  bool DeleteDocument(std::string_view file_id);

  // Physically drops tombstoned documents' postings and renumbers DocIds. Invalidates every
  // previously returned DocId.
  void Compact();

  // Null when the term is absent. May contain postings for deleted documents — callers that
  // need live-only results must filter with IsLive().
  const PostingList* Lookup(std::string_view term) const;

  // n(t) counting live documents only, so BM25's IDF isn't skewed by tombstones.
  std::size_t DocumentFrequency(std::string_view term) const;

  bool IsLive(DocId doc_id) const;

  std::size_t DocumentCount() const { return live_documents_; }
  std::size_t UniqueTerms() const { return postings_.size(); }

  std::uint32_t DocumentLength(DocId doc_id) const;
  double AverageDocumentLength() const;

  const std::string& FileId(DocId doc_id) const;

  // Live doc ids in ascending order — the universe a NOT clause complements.
  std::vector<DocId> AllDocuments() const;

  // Live documents carrying `value` for `field`, ascending. Empty when unknown.
  std::vector<DocId> DocumentsWithField(std::string_view field, std::string_view value) const;

  // Every distinct surface form seen, with how many times it occurred — the vocabulary that
  // feeds autocomplete and spell correction.
  const std::unordered_map<std::string, std::uint64_t>& Vocabulary() const { return vocabulary_; }

  // Delta+varint encoded snapshot of the whole index (ADR-0007). Round-trips through Load().
  std::string Serialize() const;
  bool Load(std::string_view bytes);

 private:
  struct DocumentMeta {
    std::string file_id;
    std::uint32_t length;
    bool deleted;
    std::map<std::string, std::string> fields;
  };

  std::unordered_map<std::string, PostingList, TermHash, std::equal_to<>> postings_;
  std::vector<DocumentMeta> docs_;
  std::unordered_map<std::string, DocId, TermHash, std::equal_to<>> by_file_id_;
  std::unordered_map<std::string, std::uint64_t> vocabulary_;
  std::uint64_t live_length_ = 0;
  std::size_t live_documents_ = 0;
};

}  // namespace atlas::search

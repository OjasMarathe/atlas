#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/query/parser.h"
#include "search/bk_tree.h"
#include "search/inverted_index.h"
#include "search/ranker.h"
#include "search/trie.h"
#include "search/types.h"

namespace atlas::search {

struct SearchHit {
  std::string file_id;
  double score;
};

struct ShardStatistics {
  std::size_t document_count;
  std::size_t unique_terms;
  double average_document_length;
};

// One search shard: index documents, then answer queries over *these* documents only.
// Fan-out across shards and merging of their local top-Ks is the Phase 4 coordinator's job
// (ADR-0006) — nothing here knows other shards exist.
class SearchEngine {
 public:
  // Indexes (or re-indexes) a document. `fields` are exact-match attributes for filtering.
  DocId IndexDocument(std::string file_id, std::string_view text,
                      const std::map<std::string, std::string>& fields = {});

  bool DeleteDocument(std::string_view file_id);

  // Drops tombstoned documents' postings. Rebuilds the suggestion structures with it.
  void Compact();

  // Parses the query (boolean operators, "quoted phrases", field:value filters), selects
  // matching documents, then ranks them with BM25. `error` receives a parse error when the
  // query is malformed, in which case the result is empty.
  std::vector<SearchHit> Search(std::string_view query, std::size_t top_k,
                                std::string* error = nullptr) const;

  // Autocomplete: indexed words starting with `prefix`, most frequent first.
  std::vector<Completion> Suggest(std::string_view prefix, std::size_t limit) const;

  // Spell correction: indexed words within `max_distance` edits of `word`, nearest first.
  // Empty when the word is already indexed — there is nothing to correct.
  std::vector<Suggestion> DidYouMean(std::string_view word, std::size_t max_distance = 2) const;

  ShardStatistics Stats() const;

  const InvertedIndex& index() const { return index_; }

 private:
  // Resolves a parsed query tree to the sorted set of live documents that satisfy it.
  std::vector<DocId> Evaluate(const query::Node* node) const;

  // Documents where the phrase's terms appear at consecutive positions.
  std::vector<DocId> EvaluatePhrase(const query::Node* node) const;

  void RebuildSuggesters();

  InvertedIndex index_;
  Trie completions_;
  BkTree vocabulary_;
};

}  // namespace atlas::search

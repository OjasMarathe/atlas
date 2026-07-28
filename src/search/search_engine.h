#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common/query/parser.h"
#include "search/inverted_index.h"
#include "search/ranker.h"
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
  DocId IndexDocument(std::string file_id, std::string_view text);

  // Parses the query (boolean operators + implicit OR), selects matching documents, then ranks
  // them with BM25. `error` receives a parse error when the query is malformed, in which case
  // the result is empty.
  std::vector<SearchHit> Search(std::string_view query, std::size_t top_k,
                                std::string* error = nullptr) const;

  ShardStatistics Stats() const;

  const InvertedIndex& index() const { return index_; }

 private:
  // Resolves a parsed query tree to the sorted set of documents that satisfy it.
  std::vector<DocId> Evaluate(const query::Node* node) const;

  InvertedIndex index_;
};

}  // namespace atlas::search

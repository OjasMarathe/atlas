#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
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
  std::string snippet;  // a window of the document around the first matching term
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

  // Tombstones the document. Compacts automatically once tombstones exceed
  // kCompactionThreshold of the index, so a long-lived shard doesn't grow without bound.
  bool DeleteDocument(std::string_view file_id);

  // Fraction of tombstoned documents that triggers an automatic compaction on delete.
  static constexpr double kCompactionThreshold = 0.3;

  // Drops tombstoned documents' postings. Rebuilds the suggestion structures with it.
  void Compact();

  // Parses the query (boolean operators, "quoted phrases", field:value filters), selects
  // matching documents, then ranks them with BM25. `error` receives a parse error when the
  // query is malformed, in which case the result is empty.
  //
  // `global`, when supplied by the Phase 4 coordinator, replaces this shard's collection
  // statistics so scores are comparable with other shards' (ADR-0010).
  std::vector<SearchHit> Search(std::string_view query, std::size_t top_k,
                                std::string* error = nullptr,
                                const GlobalStatistics* global = nullptr) const;

  // Analyzed query terms and this shard's n(t) for each — round 1 of a DFS query. Terms the
  // shard has never seen report 0, which is information the coordinator needs.
  std::unordered_map<std::string, std::size_t> TermFrequencies(
      const std::vector<std::string>& terms) const;

  // The analyzed positive terms of `query`, so the coordinator can ask every shard about the
  // same term set without duplicating the analyzer.
  static std::vector<std::string> QueryTerms(std::string_view query);

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

  // A window of the document's text around its first occurrence of any query term.
  std::string BuildSnippet(DocId doc_id, const std::vector<std::string>& terms) const;

  void RebuildSuggesters();

  // Deleting changes the vocabulary, so the trie and BK-tree go stale. Rebuilding them on every
  // delete would be wasteful, so we mark them dirty and rebuild lazily on the next suggestion —
  // mutable because that repair is invisible to callers of the const query methods.
  void EnsureSuggestersFresh() const;

  InvertedIndex index_;
  mutable Trie completions_;
  mutable BkTree vocabulary_;
  mutable bool suggesters_dirty_ = false;
};

}  // namespace atlas::search

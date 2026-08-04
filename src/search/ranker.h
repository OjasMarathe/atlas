#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "search/inverted_index.h"
#include "search/types.h"

namespace atlas::search {

struct ScoredDocument {
  DocId doc_id;
  double score;
};

// Namespace scope rather than nested in Ranker: a defaulted `Params params = {}` constructor
// argument needs the type complete, which it isn't while the enclosing class is still being
// defined.
struct BM25Params {
  double k1 = 1.5;  // term-frequency saturation
  double b = 0.75;  // document-length normalization
};

// Corpus-wide BM25 inputs, supplied by the Phase 4 coordinator so that scores computed on
// different shards are comparable. Absent (the default) means "use this shard's own numbers",
// which is right for a single-shard search and an approximation for a merged one.
//
// Only the *collection* statistics are global. Term frequency and document length stay local
// because they are properties of the document itself, not of the corpus it sits in.
struct GlobalStatistics {
  std::size_t document_count = 0;
  double average_document_length = 0.0;
  std::unordered_map<std::string, std::size_t> document_frequency;  // n(t), corpus-wide

  bool valid() const { return document_count > 0 && average_document_length > 0.0; }
};

// BM25 ranking over a shard's local index. The formula, the smoothed non-negative IDF, and the
// default parameters all follow docs/concepts/bm25.md.
//
// By default, N, n(t) and avgdl come from *this shard*, so scores are comparable only within it
// — merging raw shard scores is therefore an approximation. Phase 4's coordinator closes that
// gap by collecting those statistics from every shard, summing them, and passing them back in
// as `GlobalStatistics` (ADR-0010). With them set, two documents on different shards get the
// same score they would have got from one big index.
class Ranker {
 public:
  explicit Ranker(const InvertedIndex& index, BM25Params params = {})
      : index_(index), params_(params) {}

  Ranker(const InvertedIndex& index, const GlobalStatistics* global, BM25Params params = {})
      : index_(index),
        params_(params),
        global_(global != nullptr && global->valid() ? global : nullptr) {}

  // ln(1 + (N - n(t) + 0.5) / (n(t) + 0.5)) — always >= 0, unlike classic BM25 IDF.
  double InverseDocumentFrequency(std::string_view term) const;

  // Scores `candidates` against `query_terms` and returns the best `top_k`, highest first.
  // Ties break on ascending doc_id so results are deterministic.
  std::vector<ScoredDocument> TopK(const std::vector<std::string>& query_terms,
                                   const std::vector<DocId>& candidates, std::size_t top_k) const;

 private:
  const InvertedIndex& index_;
  BM25Params params_;
  const GlobalStatistics* global_ = nullptr;  // null => rank with this shard's own statistics
};

}  // namespace atlas::search

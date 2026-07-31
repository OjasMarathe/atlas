#pragma once

#include <cstddef>
#include <string>
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

// BM25 ranking over a shard's local index. The formula, the smoothed non-negative IDF, and the
// default parameters all follow docs/concepts/bm25.md.
//
// Scores are only comparable *within* a shard: N, n(t) and avgdl are local statistics, so the
// coordinator merging shard results in Phase 4 is combining approximations. That trade-off is
// accepted for M1 and documented in bm25.md and ADR-0006.
class Ranker {
 public:
  explicit Ranker(const InvertedIndex& index, BM25Params params = {})
      : index_(index), params_(params) {}

  // ln(1 + (N - n(t) + 0.5) / (n(t) + 0.5)) — always >= 0, unlike classic BM25 IDF.
  double InverseDocumentFrequency(std::string_view term) const;

  // Scores `candidates` against `query_terms` and returns the best `top_k`, highest first.
  // Ties break on ascending doc_id so results are deterministic.
  std::vector<ScoredDocument> TopK(const std::vector<std::string>& query_terms,
                                   const std::vector<DocId>& candidates, std::size_t top_k) const;

 private:
  const InvertedIndex& index_;
  BM25Params params_;
};

}  // namespace atlas::search

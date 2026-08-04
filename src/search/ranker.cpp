#include "search/ranker.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace atlas::search {

double Ranker::InverseDocumentFrequency(std::string_view term) const {
  double n = static_cast<double>(index_.DocumentFrequency(term));
  double total = static_cast<double>(index_.DocumentCount());
  if (global_ != nullptr) {
    total = static_cast<double>(global_->document_count);
    // A term the coordinator did not ask about falls back to the local count rather than 0:
    // n(t)=0 would inflate IDF to its maximum and make an unknown term the strongest signal
    // in the query.
    const auto it = global_->document_frequency.find(std::string(term));
    if (it != global_->document_frequency.end()) n = static_cast<double>(it->second);
  }
  return std::log(1.0 + (total - n + 0.5) / (n + 0.5));
}

std::vector<ScoredDocument> Ranker::TopK(const std::vector<std::string>& query_terms,
                                         const std::vector<DocId>& candidates,
                                         std::size_t top_k) const {
  if (candidates.empty() || query_terms.empty() || top_k == 0) return {};

  const std::unordered_set<DocId> allowed(candidates.begin(), candidates.end());
  // Length normalization divides this document's length by the *corpus* average, so when the
  // coordinator supplies a global avgdl a long document is judged long relative to the whole
  // collection rather than to whatever happens to share its shard.
  const double avgdl =
      global_ != nullptr ? global_->average_document_length : index_.AverageDocumentLength();

  // Term-at-a-time: walk each term's posting list once and accumulate into a doc -> score map,
  // rather than re-scanning the index per candidate document.
  std::unordered_map<DocId, double> scores;
  for (const std::string& term : query_terms) {
    const PostingList* postings = index_.Lookup(term);
    if (postings == nullptr) continue;
    const double idf = InverseDocumentFrequency(term);
    for (const Posting& posting : *postings) {
      if (!allowed.contains(posting.doc_id)) continue;
      const double length_norm =
          avgdl > 0.0
              ? 1.0 - params_.b +
                    params_.b * (static_cast<double>(index_.DocumentLength(posting.doc_id)) / avgdl)
              : 1.0;
      const auto tf = static_cast<double>(posting.term_frequency);
      scores[posting.doc_id] += idf * (tf * (params_.k1 + 1.0)) / (tf + params_.k1 * length_norm);
    }
  }
  if (scores.empty()) return {};

  // Highest score first; ascending doc_id breaks ties so output is deterministic.
  const auto better = [](const ScoredDocument& lhs, const ScoredDocument& rhs) {
    if (lhs.score != rhs.score) return lhs.score > rhs.score;
    return lhs.doc_id < rhs.doc_id;
  };

  // Bounded min-heap of size K: keeps memory at O(K) rather than O(matches). priority_queue
  // surfaces the element that compares "least important" under its comparator, so passing
  // `better` puts the *weakest* survivor on top — one comparison to reject a new document.
  std::priority_queue<ScoredDocument, std::vector<ScoredDocument>, decltype(better)> heap(better);
  for (const auto& [doc_id, score] : scores) {
    const ScoredDocument candidate{doc_id, score};
    if (heap.size() < top_k) {
      heap.push(candidate);
    } else if (better(candidate, heap.top())) {
      heap.pop();
      heap.push(candidate);
    }
  }

  std::vector<ScoredDocument> results;
  results.reserve(heap.size());
  while (!heap.empty()) {
    results.push_back(heap.top());
    heap.pop();
  }
  std::sort(results.begin(), results.end(), better);
  return results;
}

}  // namespace atlas::search

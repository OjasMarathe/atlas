#include "search/search_engine.h"

#include <utility>

#include "search/postings.h"
#include "search/stemmer.h"
#include "search/stopwords.h"
#include "search/text_pipeline.h"
#include "search/tokenizer.h"

namespace atlas::search {
namespace {

// The query path must analyze terms exactly as the index path did, or a query looks up a term
// that was never stored. Analyze() owns that logic for documents; this applies the same two
// steps to a single query word.
std::string AnalyzeQueryTerm(std::string_view word) {
  const std::vector<std::string> tokens = Tokenize(word);
  if (tokens.size() != 1) return {};  // punctuation-only, or a word that split apart
  if (IsStopWord(tokens.front())) return {};
  return Stem(tokens.front());
}

}  // namespace

DocId SearchEngine::IndexDocument(std::string file_id, std::string_view text) {
  return index_.AddDocument(std::move(file_id), text);
}

std::vector<DocId> SearchEngine::Evaluate(const query::Node* node) const {
  if (node == nullptr) return {};

  switch (node->kind) {
    case query::Node::Kind::Term: {
      const PostingList* postings = index_.Lookup(node->term);
      if (postings == nullptr) return {};
      std::vector<DocId> docs;
      docs.reserve(postings->size());
      for (const Posting& posting : *postings) docs.push_back(posting.doc_id);
      return docs;  // already ascending: postings are appended in doc order
    }
    case query::Node::Kind::And: {
      std::vector<DocId> result = Evaluate(node->children.front().get());
      for (std::size_t i = 1; i < node->children.size() && !result.empty(); ++i) {
        result = Intersect(result, Evaluate(node->children[i].get()));
      }
      return result;
    }
    case query::Node::Kind::Or: {
      std::vector<DocId> result = Evaluate(node->children.front().get());
      for (std::size_t i = 1; i < node->children.size(); ++i) {
        result = Union(result, Evaluate(node->children[i].get()));
      }
      return result;
    }
    case query::Node::Kind::Not:
      // A bare NOT is complemented against the whole shard. Nested under AND this is wasteful
      // (the parent immediately intersects it away) but it keeps NOT a self-contained node.
      return Difference(index_.AllDocuments(), Evaluate(node->children.front().get()));
  }
  return {};
}

std::vector<SearchHit> SearchEngine::Search(std::string_view query, std::size_t top_k,
                                            std::string* error) const {
  if (error != nullptr) error->clear();

  query::Options options;
  options.analyze_term = AnalyzeQueryTerm;
  query::Result parsed = query::Parse(query, options);
  if (!parsed.ok()) {
    if (error != nullptr) *error = parsed.error;
    return {};
  }
  if (parsed.root == nullptr) return {};  // query was empty or all stop words

  const std::vector<DocId> candidates = Evaluate(parsed.root.get());
  if (candidates.empty()) return {};

  const std::vector<std::string> terms = query::PositiveTerms(parsed.root.get());
  const Ranker ranker(index_);
  const std::vector<ScoredDocument> ranked = ranker.TopK(terms, candidates, top_k);

  std::vector<SearchHit> hits;
  hits.reserve(ranked.size());
  for (const ScoredDocument& doc : ranked) {
    hits.push_back(SearchHit{index_.FileId(doc.doc_id), doc.score});
  }
  return hits;
}

ShardStatistics SearchEngine::Stats() const {
  return ShardStatistics{index_.DocumentCount(), index_.UniqueTerms(),
                         index_.AverageDocumentLength()};
}

}  // namespace atlas::search

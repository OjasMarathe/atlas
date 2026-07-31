#include "search/search_engine.h"

#include <algorithm>
#include <utility>

#include "search/postings.h"
#include "search/stemmer.h"
#include "search/stopwords.h"
#include "search/text_pipeline.h"
#include "search/tokenizer.h"

namespace atlas::search {
namespace {

// The query path must analyze terms exactly as the index path did, or a query looks up a term
// that was never stored. Analyze() owns that logic for documents; this applies the same steps
// to a single query word.
//
// A query word can yield several index terms: "write-ahead" tokenizes to write + ahead, exactly
// as the document did. Returning both (the parser ANDs them) keeps the query and document paths
// symmetric — dropping such words instead made "write-ahead" unsearchable.
std::vector<std::string> AnalyzeQueryTerm(std::string_view word) {
  std::vector<std::string> terms;
  for (const std::string& token : Tokenize(word)) {
    if (IsStopWord(token)) continue;
    terms.push_back(Stem(token));
  }
  return terms;
}

// Binary search a posting list (sorted by doc_id) for one document.
const Posting* FindPosting(const PostingList& postings, DocId doc_id) {
  const auto it =
      std::lower_bound(postings.begin(), postings.end(), doc_id,
                       [](const Posting& posting, DocId id) { return posting.doc_id < id; });
  if (it == postings.end() || it->doc_id != doc_id) return nullptr;
  return &*it;
}

}  // namespace

DocId SearchEngine::IndexDocument(std::string file_id, std::string_view text,
                                  const std::map<std::string, std::string>& fields) {
  const DocId doc_id = index_.IndexDocument(std::move(file_id), text, fields);
  // Feed the suggesters incrementally; a full rebuild only happens on Compact().
  for (const Term& term : Analyze(text)) {
    const auto it = index_.Vocabulary().find(term.surface);
    completions_.Insert(term.surface, it == index_.Vocabulary().end() ? 1 : it->second);
    vocabulary_.Insert(term.surface);
  }
  return doc_id;
}

bool SearchEngine::DeleteDocument(std::string_view file_id) {
  return index_.DeleteDocument(file_id);
}

void SearchEngine::Compact() {
  index_.Compact();
  RebuildSuggesters();
}

void SearchEngine::RebuildSuggesters() {
  completions_ = Trie{};
  vocabulary_ = BkTree{};
  for (const auto& [word, frequency] : index_.Vocabulary()) {
    completions_.Insert(word, frequency);
    vocabulary_.Insert(word);
  }
}

std::vector<DocId> SearchEngine::EvaluatePhrase(const query::Node* node) const {
  // Every term must be present, so start from the cheapest possible candidate set: the
  // intersection of the terms' posting lists. Only then pay for position checks.
  std::vector<const PostingList*> lists;
  lists.reserve(node->children.size());
  for (const auto& child : node->children) {
    const PostingList* postings = index_.Lookup(child->term);
    if (postings == nullptr) return {};
    lists.push_back(postings);
  }

  std::vector<DocId> candidates;
  for (const Posting& posting : *lists.front()) {
    if (index_.IsLive(posting.doc_id)) candidates.push_back(posting.doc_id);
  }
  for (std::size_t i = 1; i < lists.size() && !candidates.empty(); ++i) {
    std::vector<DocId> docs;
    docs.reserve(lists[i]->size());
    for (const Posting& posting : *lists[i]) docs.push_back(posting.doc_id);
    candidates = Intersect(candidates, docs);
  }

  // A document matches when some position p aligns every term: term i must occur at
  // p + its offset within the phrase.
  std::vector<DocId> matches;
  for (const DocId doc_id : candidates) {
    const Posting* first = FindPosting(*lists.front(), doc_id);
    if (first == nullptr) continue;
    const std::uint32_t first_offset = node->children.front()->phrase_offset;

    for (const std::uint32_t position : first->positions) {
      if (position < first_offset) continue;
      const std::uint32_t anchor = position - first_offset;
      bool aligned = true;
      for (std::size_t i = 1; i < lists.size() && aligned; ++i) {
        const Posting* posting = FindPosting(*lists[i], doc_id);
        if (posting == nullptr) {
          aligned = false;
          break;
        }
        const std::uint32_t expected = anchor + node->children[i]->phrase_offset;
        aligned =
            std::binary_search(posting->positions.begin(), posting->positions.end(), expected);
      }
      if (aligned) {
        matches.push_back(doc_id);
        break;
      }
    }
  }
  return matches;
}

std::vector<DocId> SearchEngine::Evaluate(const query::Node* node) const {
  if (node == nullptr) return {};

  switch (node->kind) {
    case query::Node::Kind::Term: {
      const PostingList* postings = index_.Lookup(node->term);
      if (postings == nullptr) return {};
      std::vector<DocId> docs;
      docs.reserve(postings->size());
      for (const Posting& posting : *postings) {
        // Tombstoned documents keep their postings until Compact(), so filter here — every set
        // operation above this point then works on live ids only.
        if (index_.IsLive(posting.doc_id)) docs.push_back(posting.doc_id);
      }
      return docs;  // ascending: postings are appended in doc order
    }
    case query::Node::Kind::Phrase:
      return EvaluatePhrase(node);
    case query::Node::Kind::Field:
      return index_.DocumentsWithField(node->field, node->term);
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
  if (terms.empty()) {
    // Nothing to rank on. If the query nonetheless *selected* documents by filter
    // ("author:ojas"), that's a legitimate request for a set — return it in document order with
    // no score. A bare NOT selects nothing meaningful, so it still returns nothing.
    if (!query::HasPositiveFilter(parsed.root.get())) return {};
    std::vector<SearchHit> filtered;
    filtered.reserve(std::min(top_k, candidates.size()));
    for (const DocId doc_id : candidates) {
      if (filtered.size() >= top_k) break;
      filtered.push_back(SearchHit{index_.FileId(doc_id), 0.0});
    }
    return filtered;
  }

  const Ranker ranker(index_);
  const std::vector<ScoredDocument> ranked = ranker.TopK(terms, candidates, top_k);

  std::vector<SearchHit> hits;
  hits.reserve(ranked.size());
  for (const ScoredDocument& doc : ranked) {
    hits.push_back(SearchHit{index_.FileId(doc.doc_id), doc.score});
  }
  return hits;
}

std::vector<Completion> SearchEngine::Suggest(std::string_view prefix, std::size_t limit) const {
  const std::vector<std::string> analyzed = Tokenize(prefix);
  if (analyzed.empty()) return {};
  // Autocomplete works on the surface form, not the stem: a user typing "replic" expects
  // "replication", not the stem it happens to share.
  return completions_.Complete(analyzed.back(), limit);
}

std::vector<Suggestion> SearchEngine::DidYouMean(std::string_view word,
                                                 std::size_t max_distance) const {
  const std::vector<std::string> tokens = Tokenize(word);
  if (tokens.size() != 1) return {};
  if (index_.Vocabulary().contains(tokens.front())) return {};  // spelled fine already
  std::vector<Suggestion> found = vocabulary_.Search(tokens.front(), max_distance);
  // A correction that is itself unindexed is useless; Search() already only returns indexed
  // words, so just drop the exact-match case defensively.
  std::erase_if(found, [&](const Suggestion& s) { return s.distance == 0; });
  return found;
}

ShardStatistics SearchEngine::Stats() const {
  return ShardStatistics{index_.DocumentCount(), index_.UniqueTerms(),
                         index_.AverageDocumentLength()};
}

}  // namespace atlas::search

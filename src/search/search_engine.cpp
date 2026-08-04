#include "search/search_engine.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>
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
  if (!index_.DeleteDocument(file_id)) return false;
  // The vocabulary shrank, so the suggesters no longer reflect it.
  suggesters_dirty_ = true;

  const std::size_t total = index_.DocumentCount() + index_.DeletedDocumentCount();
  if (total > 0 && static_cast<double>(index_.DeletedDocumentCount()) / static_cast<double>(total) >
                       kCompactionThreshold) {
    Compact();
  }
  return true;
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
  suggesters_dirty_ = false;
}

void SearchEngine::EnsureSuggestersFresh() const {
  if (!suggesters_dirty_) return;
  const_cast<SearchEngine*>(this)->RebuildSuggesters();
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
  std::vector<const Posting*> resolved(lists.size(), nullptr);
  for (const DocId doc_id : candidates) {
    // Resolve each term's posting for this document once. Which posting holds a term in this
    // document doesn't depend on the anchor being tested, so looking it up inside the position
    // loop repeats the same binary search for every candidate position.
    bool all_present = true;
    for (std::size_t i = 0; i < lists.size(); ++i) {
      resolved[i] = FindPosting(*lists[i], doc_id);
      if (resolved[i] == nullptr) {
        all_present = false;
        break;
      }
    }
    if (!all_present) continue;

    const std::uint32_t first_offset = node->children.front()->phrase_offset;
    for (const std::uint32_t position : resolved.front()->positions) {
      if (position < first_offset) continue;
      const std::uint32_t anchor = position - first_offset;
      bool aligned = true;
      for (std::size_t i = 1; i < lists.size() && aligned; ++i) {
        const std::uint32_t expected = anchor + node->children[i]->phrase_offset;
        aligned = std::binary_search(resolved[i]->positions.begin(), resolved[i]->positions.end(),
                                     expected);
      }
      if (aligned) {
        matches.push_back(doc_id);
        break;
      }
    }
  }
  return matches;
}

std::string SearchEngine::BuildSnippet(DocId doc_id, const std::vector<std::string>& terms) const {
  const std::string& text = index_.Text(doc_id);
  if (text.empty() || terms.empty()) return {};

  const std::unordered_set<std::string> wanted(terms.begin(), terms.end());

  // Re-tokenize on demand rather than storing byte offsets for every term: only the handful of
  // documents that reached the top-K ever pay for it.
  const std::vector<LocatedToken> tokens = TokenizeWithOffsets(text);
  std::size_t match_begin = std::string::npos;
  for (const LocatedToken& token : tokens) {
    if (IsStopWord(token.text)) continue;
    if (wanted.contains(Stem(token.text))) {
      match_begin = token.begin;
      break;
    }
  }
  if (match_begin == std::string::npos) return {};

  // Center a window on the match, then snap both ends outward to whitespace so the snippet
  // doesn't start or end mid-word. The snap is bounded: text with no spaces (a long token, a
  // base64 blob) would otherwise drag the window across the whole document.
  constexpr std::size_t kContext = 90;
  constexpr std::size_t kMaxSnap = 20;
  std::size_t begin = match_begin > kContext ? match_begin - kContext : 0;
  std::size_t end = std::min(text.size(), match_begin + kContext);

  const std::size_t snap_floor = begin > kMaxSnap ? begin - kMaxSnap : 0;
  while (begin > snap_floor && (std::isspace(static_cast<unsigned char>(text[begin])) == 0)) {
    --begin;
  }
  const std::size_t snap_ceiling = std::min(text.size(), end + kMaxSnap);
  while (end < snap_ceiling && (std::isspace(static_cast<unsigned char>(text[end])) == 0)) ++end;

  std::string snippet;
  if (begin > 0) snippet += "...";
  // Collapse newlines so a snippet stays on one line.
  for (std::size_t i = begin; i < end; ++i) {
    const char ch = text[i];
    snippet.push_back(ch == '\n' || ch == '\r' ? ' ' : ch);
  }
  if (end < text.size()) snippet += "...";
  return snippet;
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
                                            std::string* error,
                                            const GlobalStatistics* global) const {
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
      filtered.push_back(SearchHit{index_.FileId(doc_id), 0.0, {}});
    }
    return filtered;
  }

  const Ranker ranker(index_, global);
  const std::vector<ScoredDocument> ranked = ranker.TopK(terms, candidates, top_k);

  std::vector<SearchHit> hits;
  hits.reserve(ranked.size());
  for (const ScoredDocument& doc : ranked) {
    hits.push_back(
        SearchHit{index_.FileId(doc.doc_id), doc.score, BuildSnippet(doc.doc_id, terms)});
  }
  return hits;
}

std::vector<Completion> SearchEngine::Suggest(std::string_view prefix, std::size_t limit) const {
  EnsureSuggestersFresh();
  const std::vector<std::string> analyzed = Tokenize(prefix);
  if (analyzed.empty()) return {};
  // Autocomplete works on the surface form, not the stem: a user typing "replic" expects
  // "replication", not the stem it happens to share.
  return completions_.Complete(analyzed.back(), limit);
}

std::vector<Suggestion> SearchEngine::DidYouMean(std::string_view word,
                                                 std::size_t max_distance) const {
  EnsureSuggestersFresh();
  const std::vector<std::string> tokens = Tokenize(word);
  if (tokens.size() != 1) return {};
  if (index_.Vocabulary().contains(tokens.front())) return {};  // spelled fine already
  std::vector<Suggestion> found = vocabulary_.Search(tokens.front(), max_distance);
  // A correction that is itself unindexed is useless; Search() already only returns indexed
  // words, so just drop the exact-match case defensively.
  std::erase_if(found, [&](const Suggestion& s) { return s.distance == 0; });
  return found;
}

std::vector<std::string> SearchEngine::QueryTerms(std::string_view query) {
  query::Options options;
  options.analyze_term = AnalyzeQueryTerm;
  const query::Result parsed = query::Parse(query, options);
  if (!parsed.ok() || parsed.root == nullptr) return {};
  return query::PositiveTerms(parsed.root.get());
}

std::unordered_map<std::string, std::size_t> SearchEngine::TermFrequencies(
    const std::vector<std::string>& terms) const {
  std::unordered_map<std::string, std::size_t> frequencies;
  frequencies.reserve(terms.size());
  // Report a 0 for terms this shard has never seen rather than omitting them: the coordinator
  // sums these, and a missing entry is indistinguishable from a shard that failed to answer.
  for (const std::string& term : terms) frequencies[term] = index_.DocumentFrequency(term);
  return frequencies;
}

ShardStatistics SearchEngine::Stats() const {
  return ShardStatistics{index_.DocumentCount(), index_.UniqueTerms(),
                         index_.AverageDocumentLength()};
}

}  // namespace atlas::search

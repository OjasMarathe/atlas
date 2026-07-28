#include "search/inverted_index.h"

#include <stdexcept>
#include <utility>

#include "search/text_pipeline.h"

namespace atlas::search {

DocId InvertedIndex::AddDocument(std::string file_id, std::string_view text) {
  const std::vector<Term> terms = Analyze(text);
  const auto doc_id = static_cast<DocId>(docs_.size());
  docs_.push_back(DocumentMeta{std::move(file_id), static_cast<std::uint32_t>(terms.size())});
  total_length_ += terms.size();

  // Documents arrive with monotonically increasing doc ids, so appending to each term's list
  // keeps every posting list sorted by doc_id without an explicit sort.
  for (const Term& term : terms) {
    PostingList& list = postings_[term.text];
    if (list.empty() || list.back().doc_id != doc_id) {
      list.push_back(Posting{doc_id, 0, {}});
    }
    Posting& posting = list.back();
    ++posting.term_frequency;
    posting.positions.push_back(term.position);
  }
  return doc_id;
}

const PostingList* InvertedIndex::Lookup(std::string_view term) const {
  const auto it = postings_.find(term);
  return it == postings_.end() ? nullptr : &it->second;
}

std::size_t InvertedIndex::DocumentFrequency(std::string_view term) const {
  const PostingList* list = Lookup(term);
  return list == nullptr ? 0 : list->size();
}

std::uint32_t InvertedIndex::DocumentLength(DocId doc_id) const {
  if (doc_id >= docs_.size()) throw std::out_of_range("InvertedIndex::DocumentLength: bad DocId");
  return docs_[doc_id].length;
}

double InvertedIndex::AverageDocumentLength() const {
  if (docs_.empty()) return 0.0;
  return static_cast<double>(total_length_) / static_cast<double>(docs_.size());
}

const std::string& InvertedIndex::FileId(DocId doc_id) const {
  if (doc_id >= docs_.size()) throw std::out_of_range("InvertedIndex::FileId: bad DocId");
  return docs_[doc_id].file_id;
}

std::vector<DocId> InvertedIndex::AllDocuments() const {
  std::vector<DocId> ids(docs_.size());
  for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = static_cast<DocId>(i);
  return ids;
}

}  // namespace atlas::search

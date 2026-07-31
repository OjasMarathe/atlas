#include "search/inverted_index.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "search/posting_codec.h"
#include "search/text_pipeline.h"

namespace atlas::search {

DocId InvertedIndex::IndexDocument(std::string file_id, std::string_view text,
                                   const std::map<std::string, std::string>& fields) {
  // Re-indexing supersedes: tombstone the old document so its postings stop matching, then
  // append a fresh one. Rewriting the old postings in place would mean touching every term the
  // document contained, which is exactly what the append-only layout is designed to avoid.
  DeleteDocument(file_id);

  const std::vector<Term> terms = Analyze(text);
  const auto doc_id = static_cast<DocId>(docs_.size());
  docs_.push_back(DocumentMeta{file_id, static_cast<std::uint32_t>(terms.size()), false, fields,
                               std::string(text)});
  by_file_id_[std::move(file_id)] = doc_id;
  live_length_ += terms.size();
  ++live_documents_;

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
    ++vocabulary_[term.surface];
  }
  return doc_id;
}

bool InvertedIndex::DeleteDocument(std::string_view file_id) {
  const auto it = by_file_id_.find(file_id);
  if (it == by_file_id_.end()) return false;

  DocumentMeta& meta = docs_[it->second];
  meta.deleted = true;
  live_length_ -= meta.length;
  --live_documents_;

  // Give back this document's contribution to the vocabulary, so autocomplete and spell
  // correction stop offering words that no live document contains. Re-analyzing the retained
  // text is what makes this possible without storing per-document surface forms.
  for (const Term& term : Analyze(meta.text)) {
    const auto entry = vocabulary_.find(term.surface);
    if (entry == vocabulary_.end()) continue;
    if (entry->second <= 1) {
      vocabulary_.erase(entry);
    } else {
      --entry->second;
    }
  }

  by_file_id_.erase(it);
  return true;
}

void InvertedIndex::Compact() {
  // Work out the renumbering first, and keep an explicit `dropped` snapshot, so rewriting the
  // posting lists never has to read from docs_ while it's being rebuilt.
  std::vector<DocId> remap(docs_.size(), 0);
  std::vector<bool> dropped(docs_.size(), true);
  DocId next_id = 0;
  for (std::size_t i = 0; i < docs_.size(); ++i) {
    if (docs_[i].deleted) continue;
    dropped[i] = false;
    remap[i] = next_id++;
  }

  for (auto it = postings_.begin(); it != postings_.end();) {
    PostingList kept;
    kept.reserve(it->second.size());
    for (Posting& posting : it->second) {
      if (dropped[posting.doc_id]) continue;
      posting.doc_id = remap[posting.doc_id];
      kept.push_back(std::move(posting));
    }
    if (kept.empty()) {
      it = postings_.erase(it);  // every document holding this term is gone
    } else {
      it->second = std::move(kept);
      ++it;
    }
  }

  std::vector<DocumentMeta> live;
  live.reserve(live_documents_);
  for (std::size_t i = 0; i < docs_.size(); ++i) {
    if (!dropped[i]) live.push_back(std::move(docs_[i]));
  }
  docs_ = std::move(live);

  by_file_id_.clear();
  for (std::size_t i = 0; i < docs_.size(); ++i) {
    by_file_id_[docs_[i].file_id] = static_cast<DocId>(i);
  }
  // vocabulary_ needs no work here: DeleteDocument already gave back each tombstoned document's
  // contribution, so by the time we compact the counts are already correct.
}

const PostingList* InvertedIndex::Lookup(std::string_view term) const {
  const auto it = postings_.find(term);
  return it == postings_.end() ? nullptr : &it->second;
}

std::size_t InvertedIndex::DocumentFrequency(std::string_view term) const {
  const PostingList* list = Lookup(term);
  if (list == nullptr) return 0;
  std::size_t live = 0;
  for (const Posting& posting : *list) {
    if (IsLive(posting.doc_id)) ++live;
  }
  return live;
}

bool InvertedIndex::IsLive(DocId doc_id) const {
  return doc_id < docs_.size() && !docs_[doc_id].deleted;
}

std::uint32_t InvertedIndex::DocumentLength(DocId doc_id) const {
  if (doc_id >= docs_.size()) throw std::out_of_range("InvertedIndex::DocumentLength: bad DocId");
  return docs_[doc_id].length;
}

double InvertedIndex::AverageDocumentLength() const {
  if (live_documents_ == 0) return 0.0;
  return static_cast<double>(live_length_) / static_cast<double>(live_documents_);
}

const std::string& InvertedIndex::FileId(DocId doc_id) const {
  if (doc_id >= docs_.size()) throw std::out_of_range("InvertedIndex::FileId: bad DocId");
  return docs_[doc_id].file_id;
}

const std::string& InvertedIndex::Text(DocId doc_id) const {
  if (doc_id >= docs_.size()) throw std::out_of_range("InvertedIndex::Text: bad DocId");
  return docs_[doc_id].text;
}

void InvertedIndex::ForEachPostingList(
    const std::function<void(const std::string&, const PostingList&)>& visit) const {
  for (const auto& [term, list] : postings_) visit(term, list);
}

void InvertedIndex::SetPostingList(std::string term, PostingList postings) {
  postings_[std::move(term)] = std::move(postings);
}

std::vector<DocId> InvertedIndex::AllDocuments() const {
  std::vector<DocId> ids;
  ids.reserve(live_documents_);
  for (std::size_t i = 0; i < docs_.size(); ++i) {
    if (!docs_[i].deleted) ids.push_back(static_cast<DocId>(i));
  }
  return ids;
}

std::vector<DocId> InvertedIndex::DocumentsWithField(std::string_view field,
                                                     std::string_view value) const {
  std::vector<DocId> ids;
  for (std::size_t i = 0; i < docs_.size(); ++i) {
    if (docs_[i].deleted) continue;
    const auto it = docs_[i].fields.find(std::string(field));
    if (it != docs_[i].fields.end() && it->second == value) {
      ids.push_back(static_cast<DocId>(i));  // ascending by construction
    }
  }
  return ids;
}

std::size_t InvertedIndex::UncompressedPostingBytes() const {
  std::size_t total = 0;
  for (const auto& [term, list] : postings_) {
    (void)term;
    for (const Posting& posting : list) {
      total += 3 * sizeof(std::uint32_t) + posting.positions.size() * sizeof(std::uint32_t);
    }
  }
  return total;
}

std::string InvertedIndex::SerializeMetadata() const {
  std::string out;
  PutVarint(docs_.size(), &out);
  for (const DocumentMeta& doc : docs_) {
    PutVarint(doc.file_id.size(), &out);
    out += doc.file_id;
    PutVarint(doc.length, &out);
    PutVarint(doc.deleted ? 1 : 0, &out);
    PutVarint(doc.text.size(), &out);
    out += doc.text;
    PutVarint(doc.fields.size(), &out);
    for (const auto& [key, value] : doc.fields) {
      PutVarint(key.size(), &out);
      out += key;
      PutVarint(value.size(), &out);
      out += value;
    }
  }

  PutVarint(vocabulary_.size(), &out);
  for (const auto& [word, count] : vocabulary_) {
    PutVarint(word.size(), &out);
    out += word;
    PutVarint(count, &out);
  }
  return out;
}

std::string InvertedIndex::Serialize() const {
  // Metadata first, then every posting list — the same two parts index_store.h writes to
  // separate column families, just concatenated into one buffer.
  std::string out = SerializeMetadata();
  PutVarint(postings_.size(), &out);
  for (const auto& [term, list] : postings_) {
    PutVarint(term.size(), &out);
    out += term;
    const std::string encoded = EncodePostingList(list);
    PutVarint(encoded.size(), &out);
    out += encoded;
  }
  return out;
}

namespace {

// Reads a varint-length-prefixed string. Returns false if the length runs past the buffer.
bool GetString(std::string_view in, std::size_t* offset, std::string* out) {
  std::uint64_t length = 0;
  if (!GetVarint(in, offset, &length)) return false;
  if (*offset + length > in.size()) return false;
  out->assign(in.substr(*offset, length));
  *offset += length;
  return true;
}

}  // namespace

namespace {

// Parses the metadata section (document table + vocabulary) starting at *offset.
bool ParseMetadata(std::string_view bytes, std::size_t* offset, InvertedIndex::Snapshot* out) {
  std::uint64_t doc_count = 0;
  if (!GetVarint(bytes, offset, &doc_count)) return false;
  // Every record costs at least a byte, so a count exceeding the remaining buffer is corruption.
  // Checking up front turns a damaged file into a clean `false` instead of a huge allocation.
  if (doc_count > bytes.size() - *offset) return false;
  for (std::uint64_t i = 0; i < doc_count; ++i) {
    InvertedIndex::DocumentRecord record;
    std::uint64_t length = 0;
    std::uint64_t deleted = 0;
    std::uint64_t field_count = 0;
    if (!GetString(bytes, offset, &record.file_id)) return false;
    if (!GetVarint(bytes, offset, &length)) return false;
    if (!GetVarint(bytes, offset, &deleted)) return false;
    if (!GetString(bytes, offset, &record.text)) return false;
    if (!GetVarint(bytes, offset, &field_count)) return false;
    record.length = static_cast<std::uint32_t>(length);
    record.deleted = deleted != 0;
    for (std::uint64_t f = 0; f < field_count; ++f) {
      std::string key;
      std::string value;
      if (!GetString(bytes, offset, &key)) return false;
      if (!GetString(bytes, offset, &value)) return false;
      record.fields.emplace(std::move(key), std::move(value));
    }
    out->documents.push_back(std::move(record));
  }

  std::uint64_t vocabulary_size = 0;
  if (!GetVarint(bytes, offset, &vocabulary_size)) return false;
  if (vocabulary_size > bytes.size() - *offset) return false;
  for (std::uint64_t i = 0; i < vocabulary_size; ++i) {
    std::string word;
    std::uint64_t count = 0;
    if (!GetString(bytes, offset, &word)) return false;
    if (!GetVarint(bytes, offset, &count)) return false;
    out->vocabulary.emplace(std::move(word), count);
  }
  return true;
}

}  // namespace

void InvertedIndex::Adopt(Snapshot snapshot) {
  docs_.clear();
  by_file_id_.clear();
  live_length_ = 0;
  live_documents_ = 0;
  vocabulary_ = std::move(snapshot.vocabulary);

  for (DocumentRecord& record : snapshot.documents) {
    DocumentMeta meta{std::move(record.file_id), record.length, record.deleted,
                      std::move(record.fields), std::move(record.text)};
    if (!meta.deleted) {
      by_file_id_[meta.file_id] = static_cast<DocId>(docs_.size());
      live_length_ += meta.length;
      ++live_documents_;
    }
    docs_.push_back(std::move(meta));
  }
}

bool InvertedIndex::LoadMetadata(std::string_view bytes) {
  Snapshot snapshot;
  std::size_t offset = 0;
  if (!ParseMetadata(bytes, &offset, &snapshot)) return false;
  Adopt(std::move(snapshot));
  postings_.clear();  // the caller repopulates these via SetPostingList
  return true;
}

bool InvertedIndex::Load(std::string_view bytes) {
  Snapshot snapshot;
  std::size_t offset = 0;
  if (!ParseMetadata(bytes, &offset, &snapshot)) return false;

  std::unordered_map<std::string, PostingList, TermHash, std::equal_to<>> postings;
  std::uint64_t term_count = 0;
  if (!GetVarint(bytes, &offset, &term_count)) return false;
  if (term_count > bytes.size() - offset) return false;
  for (std::uint64_t i = 0; i < term_count; ++i) {
    std::string term;
    std::string encoded;
    if (!GetString(bytes, &offset, &term)) return false;
    if (!GetString(bytes, &offset, &encoded)) return false;
    PostingList list;
    if (!DecodePostingList(encoded, &list)) return false;
    postings.emplace(std::move(term), std::move(list));
  }

  // Only commit once the whole buffer parsed cleanly.
  Adopt(std::move(snapshot));
  postings_ = std::move(postings);
  return true;
}

}  // namespace atlas::search

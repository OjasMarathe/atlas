#include "search/posting_codec.h"

namespace atlas::search {

void PutVarint(std::uint64_t value, std::string* out) {
  while (value >= 0x80) {
    out->push_back(static_cast<char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out->push_back(static_cast<char>(value));
}

bool GetVarint(std::string_view in, std::size_t* offset, std::uint64_t* value) {
  std::uint64_t result = 0;
  int shift = 0;
  while (*offset < in.size()) {
    const auto byte = static_cast<std::uint8_t>(in[*offset]);
    ++*offset;
    if (shift > 63) return false;  // more bytes than a uint64 can hold
    result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
    shift += 7;
  }
  return false;  // ran off the end mid-varint
}

std::string EncodePostingList(const PostingList& postings) {
  std::string out;
  PutVarint(postings.size(), &out);

  DocId previous_doc = 0;
  for (const Posting& posting : postings) {
    // Gaps, not absolute ids: the list ascends, so posting.doc_id >= previous_doc and the
    // difference is usually tiny even when the ids themselves are large.
    PutVarint(posting.doc_id - previous_doc, &out);
    previous_doc = posting.doc_id;

    PutVarint(posting.term_frequency, &out);
    PutVarint(posting.positions.size(), &out);
    std::uint32_t previous_position = 0;
    for (const std::uint32_t position : posting.positions) {
      PutVarint(position - previous_position, &out);
      previous_position = position;
    }
  }
  return out;
}

bool DecodePostingList(std::string_view bytes, PostingList* out) {
  out->clear();
  std::size_t offset = 0;
  std::uint64_t count = 0;
  if (!GetVarint(bytes, &offset, &count)) return false;
  // Sanity-check the declared count against what's actually left: every posting costs at least
  // three varint bytes, so a count larger than the remaining buffer means corruption. Without
  // this, a truncated or damaged index would reserve() an arbitrary amount before failing.
  if (count > bytes.size() - offset) return false;
  out->reserve(count);

  DocId previous_doc = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t doc_gap = 0;
    std::uint64_t term_frequency = 0;
    std::uint64_t position_count = 0;
    if (!GetVarint(bytes, &offset, &doc_gap)) return false;
    if (!GetVarint(bytes, &offset, &term_frequency)) return false;
    if (!GetVarint(bytes, &offset, &position_count)) return false;

    Posting posting;
    posting.doc_id = static_cast<DocId>(previous_doc + doc_gap);
    previous_doc = posting.doc_id;
    posting.term_frequency = static_cast<std::uint32_t>(term_frequency);
    if (position_count > bytes.size() - offset) return false;  // same guard, per posting
    posting.positions.reserve(position_count);

    std::uint32_t previous_position = 0;
    for (std::uint64_t p = 0; p < position_count; ++p) {
      std::uint64_t gap = 0;
      if (!GetVarint(bytes, &offset, &gap)) return false;
      previous_position = static_cast<std::uint32_t>(previous_position + gap);
      posting.positions.push_back(previous_position);
    }
    out->push_back(std::move(posting));
  }
  return true;
}

}  // namespace atlas::search

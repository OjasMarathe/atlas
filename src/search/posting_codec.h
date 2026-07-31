#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "search/inverted_index.h"

namespace atlas::search {

// Delta + varint posting-list compression (ADR-0007).
//
// A posting list is a sorted run of doc ids, so storing the *gap* to the previous id instead of
// the id itself keeps the numbers small; varint then spends one byte on values under 128 rather
// than a fixed four. Positions inside a posting get the same treatment. See
// concepts/posting-list-compression.md.

// Appends `value` to `out` as a LEB128 varint: 7 payload bits per byte, high bit = "more".
void PutVarint(std::uint64_t value, std::string* out);

// Reads a varint from `in` starting at `*offset`. Returns false on a truncated or overlong
// encoding, leaving `*offset` unspecified.
bool GetVarint(std::string_view in, std::size_t* offset, std::uint64_t* value);

std::string EncodePostingList(const PostingList& postings);
bool DecodePostingList(std::string_view bytes, PostingList* out);

}  // namespace atlas::search

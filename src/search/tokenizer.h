#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace atlas::search {

// Splits text into lowercased alphanumeric tokens; every other byte is a delimiter.
// ASCII-only for M1: UTF-8 multibyte sequences split on their bytes rather than folding.
std::vector<std::string> Tokenize(std::string_view text);

// A token plus where it came from in the source text.
struct LocatedToken {
  std::string text;
  std::size_t begin;  // byte offset of the token's first character
  std::size_t end;    // one past its last character
};

// Same tokenization, but keeping byte offsets. Snippet highlighting needs to map a term's token
// position back to a span of the original text, which plain Tokenize() throws away.
std::vector<LocatedToken> TokenizeWithOffsets(std::string_view text);

}  // namespace atlas::search

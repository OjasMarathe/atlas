#include "search/tokenizer.h"

#include <cctype>

namespace atlas::search {

std::vector<std::string> Tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  tokens.reserve(text.size() / 8);  // rough guess: ~8 bytes per token including delimiters
  for (const LocatedToken& token : TokenizeWithOffsets(text)) {
    tokens.push_back(token.text);
  }
  return tokens;
}

std::vector<LocatedToken> TokenizeWithOffsets(std::string_view text) {
  std::vector<LocatedToken> tokens;
  std::string current;
  std::size_t begin = 0;

  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (std::isalnum(byte) != 0) {
      if (current.empty()) begin = i;
      current.push_back(static_cast<char>(std::tolower(byte)));
    } else if (!current.empty()) {
      tokens.push_back(LocatedToken{std::move(current), begin, i});
      current.clear();
    }
  }
  if (!current.empty()) tokens.push_back(LocatedToken{std::move(current), begin, text.size()});
  return tokens;
}

}  // namespace atlas::search

#include "search/tokenizer.h"

#include <cctype>

namespace atlas::search {

std::vector<std::string> Tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  for (const char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte) != 0) {
      current.push_back(static_cast<char>(std::tolower(byte)));
    } else if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}

}  // namespace atlas::search

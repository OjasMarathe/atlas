#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace atlas::search {

// Splits text into lowercased alphanumeric tokens; every other byte is a delimiter.
// ASCII-only for M1: UTF-8 multibyte sequences split on their bytes rather than folding.
std::vector<std::string> Tokenize(std::string_view text);

}  // namespace atlas::search

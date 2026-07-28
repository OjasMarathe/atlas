#pragma once

#include <string>
#include <string_view>

namespace atlas::search {

// Porter (1980) suffix-stripping stemmer, implemented by hand.
// Expects an already-lowercased ASCII token (see Tokenize). Words of 2 or fewer letters are
// returned unchanged.
std::string Stem(std::string_view word);

}  // namespace atlas::search

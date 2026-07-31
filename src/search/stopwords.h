#pragma once

#include <string_view>

namespace atlas::search {

// True if the token is a high-frequency English function word carrying little retrieval signal.
// Expects an already-lowercased token (see Tokenize).
bool IsStopWord(std::string_view token);

}  // namespace atlas::search

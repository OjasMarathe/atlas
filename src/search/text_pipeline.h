#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::search {

struct Term {
  std::string text;
  std::uint32_t position;

  friend bool operator==(const Term&, const Term&) = default;
};

// tokenize -> drop stop words -> stem. Positions index the *pre-filter* token stream, so a
// removed stop word leaves a gap and phrase search (Phase 3b) can still tell "a b" from "a x b".
std::vector<Term> Analyze(std::string_view text);

}  // namespace atlas::search

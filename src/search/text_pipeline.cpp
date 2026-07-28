#include "search/text_pipeline.h"

#include <cstddef>

#include "search/stemmer.h"
#include "search/stopwords.h"
#include "search/tokenizer.h"

namespace atlas::search {

std::vector<Term> Analyze(std::string_view text) {
  const std::vector<std::string> tokens = Tokenize(text);
  std::vector<Term> terms;
  terms.reserve(tokens.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (IsStopWord(tokens[i])) continue;
    terms.push_back(Term{Stem(tokens[i]), static_cast<std::uint32_t>(i)});
  }
  return terms;
}

}  // namespace atlas::search

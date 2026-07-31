#include "search/tokenizer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace atlas::search {

TEST(Tokenizer, SplitsOnWhitespaceAndLowercases) {
  EXPECT_EQ(Tokenize("How Does A Filesystem"),
            (std::vector<std::string>{"how", "does", "a", "filesystem"}));
}

TEST(Tokenizer, TreatsPunctuationAsDelimiter) {
  EXPECT_EQ(Tokenize("write-ahead log, fsync()."),
            (std::vector<std::string>{"write", "ahead", "log", "fsync"}));
}

TEST(Tokenizer, KeepsDigitsAndAlphanumericRuns) {
  EXPECT_EQ(Tokenize("BM25 ranks sha256 chunks"),
            (std::vector<std::string>{"bm25", "ranks", "sha256", "chunks"}));
}

TEST(Tokenizer, CollapsesRunsOfDelimiters) {
  EXPECT_EQ(Tokenize("  raft\n\n\tconsensus  "), (std::vector<std::string>{"raft", "consensus"}));
}

TEST(Tokenizer, EmptyAndDelimiterOnlyInputYieldNoTokens) {
  EXPECT_TRUE(Tokenize("").empty());
  EXPECT_TRUE(Tokenize("---  ...\n").empty());
}

}  // namespace atlas::search

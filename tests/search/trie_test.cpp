#include "search/trie.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace atlas::search {
namespace {

std::vector<std::string> Words(const std::vector<Completion>& completions) {
  std::vector<std::string> words;
  words.reserve(completions.size());
  for (const Completion& completion : completions) words.push_back(completion.word);
  return words;
}

Trie BuildTrie() {
  Trie trie;
  trie.Insert("replication", 10);
  trie.Insert("replica", 7);
  trie.Insert("replicate", 3);
  trie.Insert("ring", 5);
  trie.Insert("read", 1);
  return trie;
}

}  // namespace

TEST(Trie, EmptyTrieCompletesNothing) {
  const Trie trie;
  EXPECT_TRUE(trie.Complete("any", 5).empty());
  EXPECT_EQ(trie.WordCount(), 0U);
}

TEST(Trie, CompletesOnlyWordsSharingThePrefix) {
  const Trie trie = BuildTrie();
  EXPECT_EQ(Words(trie.Complete("repl", 10)),
            (std::vector<std::string>{"replication", "replica", "replicate"}));
  EXPECT_EQ(Words(trie.Complete("ri", 10)), (std::vector<std::string>{"ring"}));
}

TEST(Trie, RanksCompletionsByFrequency) {
  const Trie trie = BuildTrie();
  const std::vector<Completion> completions = trie.Complete("r", 10);
  ASSERT_GE(completions.size(), 2U);
  for (std::size_t i = 1; i < completions.size(); ++i) {
    EXPECT_GE(completions[i - 1].frequency, completions[i].frequency);
  }
  EXPECT_EQ(completions.front().word, "replication");  // frequency 10, the highest
}

TEST(Trie, RespectsTheLimit) {
  const Trie trie = BuildTrie();
  EXPECT_EQ(trie.Complete("r", 2).size(), 2U);
  EXPECT_TRUE(trie.Complete("r", 0).empty());
}

TEST(Trie, UnknownPrefixYieldsNothing) {
  const Trie trie = BuildTrie();
  EXPECT_TRUE(trie.Complete("kubernetes", 5).empty());
  EXPECT_TRUE(trie.Complete("replicationz", 5).empty());
}

TEST(Trie, AWordIsACompletionOfItself) {
  const Trie trie = BuildTrie();
  EXPECT_EQ(Words(trie.Complete("ring", 5)), (std::vector<std::string>{"ring"}));
}

TEST(Trie, EmptyPrefixReturnsTheMostFrequentWords) {
  const Trie trie = BuildTrie();
  const std::vector<Completion> completions = trie.Complete("", 3);
  ASSERT_EQ(completions.size(), 3U);
  EXPECT_EQ(completions.front().word, "replication");
}

TEST(Trie, ReinsertingAWordUpdatesItsFrequencyWithoutDuplicating) {
  Trie trie;
  trie.Insert("chunk", 1);
  trie.Insert("chunk", 9);
  EXPECT_EQ(trie.WordCount(), 1U);
  const std::vector<Completion> completions = trie.Complete("chunk", 5);
  ASSERT_EQ(completions.size(), 1U);
  EXPECT_EQ(completions.front().frequency, 9U);
}

TEST(Trie, IgnoresEmptyOrZeroFrequencyInserts) {
  Trie trie;
  trie.Insert("", 5);
  trie.Insert("chunk", 0);
  EXPECT_EQ(trie.WordCount(), 0U);
}

// Equal frequencies must not produce arbitrary ordering across runs.
TEST(Trie, TiesBreakAlphabetically) {
  Trie trie;
  trie.Insert("beta", 4);
  trie.Insert("alpha", 4);
  EXPECT_EQ(Words(trie.Complete("", 5)), (std::vector<std::string>{"alpha", "beta"}));
}

}  // namespace atlas::search

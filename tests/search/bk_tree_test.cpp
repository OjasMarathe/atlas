#include "search/bk_tree.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace atlas::search {
namespace {

std::vector<std::string> Words(const std::vector<Suggestion>& suggestions) {
  std::vector<std::string> words;
  words.reserve(suggestions.size());
  for (const Suggestion& suggestion : suggestions) words.push_back(suggestion.word);
  return words;
}

const std::vector<std::string>& Vocabulary() {
  static const std::vector<std::string> kWords{"chunk",  "chunks", "check",   "replication",
                                               "replica", "ring",  "hashing", "storage"};
  return kWords;
}

BkTree BuildTree() {
  BkTree tree;
  for (const std::string& word : Vocabulary()) tree.Insert(word);
  return tree;
}

}  // namespace

TEST(EditDistance, IdenticalStringsAreZeroApart) {
  EXPECT_EQ(EditDistance("chunk", "chunk"), 0U);
  EXPECT_EQ(EditDistance("", ""), 0U);
}

TEST(EditDistance, EmptyStringCostsTheOtherLength) {
  EXPECT_EQ(EditDistance("", "chunk"), 5U);
  EXPECT_EQ(EditDistance("chunk", ""), 5U);
}

TEST(EditDistance, CountsSingleEdits) {
  EXPECT_EQ(EditDistance("chunk", "chunks"), 1U);  // insertion
  EXPECT_EQ(EditDistance("chunks", "chunk"), 1U);  // deletion
  EXPECT_EQ(EditDistance("chunk", "chank"), 1U);   // substitution
}

TEST(EditDistance, IsSymmetric) {
  EXPECT_EQ(EditDistance("kitten", "sitting"), EditDistance("sitting", "kitten"));
  EXPECT_EQ(EditDistance("kitten", "sitting"), 3U);  // the textbook example
}

TEST(BkTree, EmptyTreeFindsNothing) {
  const BkTree tree;
  EXPECT_TRUE(tree.Empty());
  EXPECT_TRUE(tree.Search("chunk", 2).empty());
}

TEST(BkTree, FindsExactMatchAtDistanceZero) {
  const BkTree tree = BuildTree();
  const std::vector<Suggestion> found = tree.Search("chunk", 0);
  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found.front().word, "chunk");
  EXPECT_EQ(found.front().distance, 0U);
}

TEST(BkTree, FindsNearMissesWithinTheDistance) {
  const BkTree tree = BuildTree();
  const std::vector<std::string> found = Words(tree.Search("chunck", 2));
  EXPECT_NE(std::find(found.begin(), found.end(), "chunk"), found.end());
  EXPECT_NE(std::find(found.begin(), found.end(), "chunks"), found.end());
}

TEST(BkTree, ResultsAreNearestFirst) {
  const BkTree tree = BuildTree();
  const std::vector<Suggestion> found = tree.Search("chunck", 3);
  ASSERT_GE(found.size(), 2U);
  for (std::size_t i = 1; i < found.size(); ++i) {
    EXPECT_LE(found[i - 1].distance, found[i].distance);
  }
}

TEST(BkTree, DistantWordsAreExcluded) {
  const BkTree tree = BuildTree();
  EXPECT_TRUE(tree.Search("kubernetes", 2).empty());
}

TEST(BkTree, DuplicateInsertsDoNotGrowTheTree) {
  BkTree tree;
  tree.Insert("chunk");
  tree.Insert("chunk");
  EXPECT_EQ(tree.Size(), 1U);
}

TEST(BkTree, IgnoresEmptyInserts) {
  BkTree tree;
  tree.Insert("");
  EXPECT_TRUE(tree.Empty());
}

// The triangle-inequality pruning is an optimization, so it must return exactly what an
// exhaustive scan of the vocabulary would. Any divergence is a bug by definition.
TEST(BkTree, PrunedSearchMatchesAnExhaustiveScan) {
  const BkTree tree = BuildTree();
  for (const std::string& query : {"chunck", "replicaton", "rng", "storag", "zzz"}) {
    for (std::size_t max_distance = 0; max_distance <= 3; ++max_distance) {
      std::vector<std::string> expected;
      for (const std::string& word : Vocabulary()) {
        if (EditDistance(query, word) <= max_distance) expected.push_back(word);
      }
      std::sort(expected.begin(), expected.end());

      std::vector<std::string> actual = Words(tree.Search(query, max_distance));
      std::sort(actual.begin(), actual.end());
      EXPECT_EQ(actual, expected) << "query=" << query << " k=" << max_distance;
    }
  }
}

}  // namespace atlas::search

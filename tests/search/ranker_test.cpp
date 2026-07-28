#include "search/ranker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "search/inverted_index.h"

namespace atlas::search {
namespace {

std::vector<std::string> Terms(std::vector<std::string> terms) { return terms; }

}  // namespace

TEST(Ranker, IdfIsHighForRareTermsAndNearZeroForUbiquitousOnes) {
  InvertedIndex index;
  for (int i = 0; i < 100; ++i) {
    // "chunk" is in every document; "raft" in only the first.
    index.AddDocument("doc" + std::to_string(i), i == 0 ? "chunk raft" : "chunk");
  }
  const Ranker ranker(index);
  EXPECT_LT(ranker.InverseDocumentFrequency("chunk"), 0.1);
  EXPECT_GT(ranker.InverseDocumentFrequency("raft"), 3.0);
}

// The smoothed ln(1 + ...) form never goes negative, unlike classic BM25 IDF.
TEST(Ranker, IdfIsNeverNegative) {
  InvertedIndex index;
  index.AddDocument("a.md", "chunk");
  index.AddDocument("b.md", "chunk");
  const Ranker ranker(index);
  EXPECT_GE(ranker.InverseDocumentFrequency("chunk"), 0.0);
  EXPECT_GE(ranker.InverseDocumentFrequency("absent"), 0.0);
}

// The headline behaviour from concepts/bm25.md: a short focused document beats a long one that
// merely accumulates occurrences by being long.
TEST(Ranker, LengthNormalizationFavoursTheShorterDocument) {
  InvertedIndex index;
  index.AddDocument("short.md", "chunk replication strategy");
  std::string padded = "chunk chunk ";
  for (int i = 0; i < 200; ++i) padded += "filler" + std::to_string(i) + " ";
  index.AddDocument("long.md", padded);

  const Ranker ranker(index);
  const std::vector<ScoredDocument> hits = ranker.TopK(Terms({"chunk"}), {0, 1}, 10);
  ASSERT_EQ(hits.size(), 2U);
  EXPECT_EQ(hits[0].doc_id, 0U) << "short doc should outrank the long one despite lower tf";
  EXPECT_GT(hits[0].score, hits[1].score);
}

TEST(Ranker, TermFrequencySaturates) {
  InvertedIndex index;
  index.AddDocument("one.md", "chunk filler filler filler filler");
  std::string many = "filler";
  for (int i = 0; i < 50; ++i) many += " chunk";
  index.AddDocument("fifty.md", many);

  const Ranker ranker(index);
  const std::vector<ScoredDocument> hits = ranker.TopK(Terms({"chunk"}), {0, 1}, 10);
  ASSERT_EQ(hits.size(), 2U);
  // 50x the occurrences must not buy anywhere near 50x the score.
  EXPECT_LT(hits[0].score, hits[1].score * 6.0);
}

TEST(Ranker, TopKTruncatesToTheBestResults) {
  InvertedIndex index;
  for (int i = 0; i < 10; ++i) index.AddDocument("doc" + std::to_string(i), "chunk");
  const Ranker ranker(index);
  EXPECT_EQ(ranker.TopK(Terms({"chunk"}), index.AllDocuments(), 3).size(), 3U);
  EXPECT_EQ(ranker.TopK(Terms({"chunk"}), index.AllDocuments(), 100).size(), 10U);
}

TEST(Ranker, ResultsAreSortedByDescendingScore) {
  InvertedIndex index;
  index.AddDocument("a.md", "chunk");
  index.AddDocument("b.md", "chunk chunk chunk filler filler filler filler filler");
  index.AddDocument("c.md", "chunk chunk filler");
  const Ranker ranker(index);
  const std::vector<ScoredDocument> hits = ranker.TopK(Terms({"chunk"}), index.AllDocuments(), 10);
  ASSERT_EQ(hits.size(), 3U);
  for (std::size_t i = 1; i < hits.size(); ++i) EXPECT_GE(hits[i - 1].score, hits[i].score);
}

TEST(Ranker, MultipleQueryTermsAccumulateScore) {
  InvertedIndex index;
  index.AddDocument("both.md", "chunk replication");
  index.AddDocument("one.md", "chunk unrelated");
  const Ranker ranker(index);
  const std::vector<ScoredDocument> hits =
      ranker.TopK(Terms({"chunk", "replic"}), index.AllDocuments(), 10);
  ASSERT_EQ(hits.size(), 2U);
  EXPECT_EQ(hits[0].doc_id, 0U) << "matching both query terms should outrank matching one";
}

TEST(Ranker, EmptyInputsProduceNoHits) {
  InvertedIndex index;
  index.AddDocument("a.md", "chunk");
  const Ranker ranker(index);
  EXPECT_TRUE(ranker.TopK({}, index.AllDocuments(), 10).empty());
  EXPECT_TRUE(ranker.TopK(Terms({"chunk"}), {}, 10).empty());
  EXPECT_TRUE(ranker.TopK(Terms({"chunk"}), index.AllDocuments(), 0).empty());
  EXPECT_TRUE(ranker.TopK(Terms({"absent"}), index.AllDocuments(), 10).empty());
}

TEST(Ranker, OnlyCandidateDocumentsAreScored) {
  InvertedIndex index;
  index.AddDocument("a.md", "chunk");
  index.AddDocument("b.md", "chunk");
  const Ranker ranker(index);
  const std::vector<ScoredDocument> hits = ranker.TopK(Terms({"chunk"}), {1}, 10);
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_EQ(hits[0].doc_id, 1U);
}

}  // namespace atlas::search

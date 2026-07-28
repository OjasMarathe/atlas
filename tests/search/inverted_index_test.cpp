#include "search/inverted_index.h"

#include <gtest/gtest.h>

#include <vector>

namespace atlas::search {

TEST(InvertedIndex, EmptyIndexHasNoStatistics) {
  const InvertedIndex index;
  EXPECT_EQ(index.DocumentCount(), 0U);
  EXPECT_EQ(index.UniqueTerms(), 0U);
  EXPECT_DOUBLE_EQ(index.AverageDocumentLength(), 0.0);
  EXPECT_EQ(index.Lookup("anything"), nullptr);
  EXPECT_EQ(index.DocumentFrequency("anything"), 0U);
}

TEST(InvertedIndex, AssignsSequentialDocIdsAndRemembersFileIds) {
  InvertedIndex index;
  EXPECT_EQ(index.AddDocument("a.md", "chunk replication"), 0U);
  EXPECT_EQ(index.AddDocument("b.md", "consistent hashing"), 1U);
  EXPECT_EQ(index.FileId(0), "a.md");
  EXPECT_EQ(index.FileId(1), "b.md");
  EXPECT_EQ(index.DocumentCount(), 2U);
}

TEST(InvertedIndex, StoresTermFrequencyAndPositions) {
  InvertedIndex index;
  // "the" is a stop word, so surviving tokens keep their original positions: chunk@1, chunk@3.
  index.AddDocument("a.md", "the chunk and chunk");

  const PostingList* postings = index.Lookup("chunk");
  ASSERT_NE(postings, nullptr);
  ASSERT_EQ(postings->size(), 1U);
  EXPECT_EQ((*postings)[0].doc_id, 0U);
  EXPECT_EQ((*postings)[0].term_frequency, 2U);
  EXPECT_EQ((*postings)[0].positions, (std::vector<std::uint32_t>{1, 3}));
}

TEST(InvertedIndex, IndexesTheStemNotTheSurfaceForm) {
  InvertedIndex index;
  index.AddDocument("a.md", "replicating chunks");
  EXPECT_EQ(index.Lookup("replicating"), nullptr);
  EXPECT_NE(index.Lookup("replic"), nullptr);  // same stem a query for "replication" produces
}

TEST(InvertedIndex, PostingListsStaySortedByDocId) {
  InvertedIndex index;
  for (int i = 0; i < 5; ++i) index.AddDocument("doc" + std::to_string(i), "chunk");

  const PostingList* postings = index.Lookup("chunk");
  ASSERT_NE(postings, nullptr);
  ASSERT_EQ(postings->size(), 5U);
  for (std::size_t i = 1; i < postings->size(); ++i) {
    EXPECT_LT((*postings)[i - 1].doc_id, (*postings)[i].doc_id);
  }
}

TEST(InvertedIndex, TracksDocumentFrequencyAndLengths) {
  InvertedIndex index;
  index.AddDocument("a.md", "chunk replication");        // 2 terms
  index.AddDocument("b.md", "chunk chunk chunk chunk");  // 4 terms, one unique
  EXPECT_EQ(index.DocumentFrequency("chunk"), 2U);       // appears in both documents
  EXPECT_EQ(index.DocumentFrequency("replic"), 1U);
  EXPECT_EQ(index.DocumentLength(0), 2U);
  EXPECT_EQ(index.DocumentLength(1), 4U);
  EXPECT_DOUBLE_EQ(index.AverageDocumentLength(), 3.0);
}

TEST(InvertedIndex, StopWordOnlyDocumentIsIndexedButContributesNoTerms) {
  InvertedIndex index;
  index.AddDocument("empty.md", "the and of a to");
  EXPECT_EQ(index.DocumentCount(), 1U);
  EXPECT_EQ(index.UniqueTerms(), 0U);
  EXPECT_EQ(index.DocumentLength(0), 0U);
}

TEST(InvertedIndex, AllDocumentsIsAscending) {
  InvertedIndex index;
  index.AddDocument("a.md", "one");
  index.AddDocument("b.md", "two");
  index.AddDocument("c.md", "three");
  EXPECT_EQ(index.AllDocuments(), (std::vector<DocId>{0, 1, 2}));
}

TEST(InvertedIndex, RejectsOutOfRangeDocIds) {
  InvertedIndex index;
  index.AddDocument("a.md", "chunk");
  EXPECT_THROW((void)index.DocumentLength(7), std::out_of_range);
  EXPECT_THROW((void)index.FileId(7), std::out_of_range);
}

}  // namespace atlas::search

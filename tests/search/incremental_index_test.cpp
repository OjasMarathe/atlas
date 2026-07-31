// Incremental indexing: re-indexing supersedes, deleting tombstones, and Compact() reclaims.
// See docs/concepts/incremental-indexing.md.

#include <gtest/gtest.h>

#include <vector>

#include "search/inverted_index.h"
#include "search/search_engine.h"

namespace atlas::search {

TEST(IncrementalIndex, ReindexingSupersedesRatherThanDuplicating) {
  InvertedIndex index;
  index.IndexDocument("a.md", "chunk replication");
  index.IndexDocument("a.md", "consistent hashing");

  EXPECT_EQ(index.DocumentCount(), 1U) << "the same file must not count twice";
  EXPECT_EQ(index.DocumentFrequency("chunk"), 0U) << "old content must stop matching";
  EXPECT_EQ(index.DocumentFrequency("hash"), 1U);
}

TEST(IncrementalIndex, DeleteRemovesTheDocumentFromResults) {
  InvertedIndex index;
  index.IndexDocument("a.md", "chunk");
  index.IndexDocument("b.md", "chunk");
  ASSERT_EQ(index.DocumentFrequency("chunk"), 2U);

  EXPECT_TRUE(index.DeleteDocument("a.md"));
  EXPECT_EQ(index.DocumentCount(), 1U);
  EXPECT_EQ(index.DocumentFrequency("chunk"), 1U) << "IDF must not count tombstoned documents";
  EXPECT_EQ(index.AllDocuments().size(), 1U);
}

TEST(IncrementalIndex, DeletingAnUnknownFileIsNotAnError) {
  InvertedIndex index;
  EXPECT_FALSE(index.DeleteDocument("nope.md"));
}

TEST(IncrementalIndex, StatisticsFollowLiveDocumentsOnly) {
  InvertedIndex index;
  index.IndexDocument("short.md", "chunk");                    // 1 term
  index.IndexDocument("long.md", "chunk chunk chunk chunk");   // 4 terms
  ASSERT_DOUBLE_EQ(index.AverageDocumentLength(), 2.5);

  index.DeleteDocument("long.md");
  EXPECT_DOUBLE_EQ(index.AverageDocumentLength(), 1.0) << "avgdl must exclude tombstones";
}

TEST(IncrementalIndex, ADeletedDocumentIsNoLongerLive) {
  InvertedIndex index;
  const DocId id = index.IndexDocument("a.md", "chunk");
  EXPECT_TRUE(index.IsLive(id));
  index.DeleteDocument("a.md");
  EXPECT_FALSE(index.IsLive(id));
}

TEST(IncrementalIndex, ReindexingAfterDeleteWorks) {
  InvertedIndex index;
  index.IndexDocument("a.md", "chunk");
  index.DeleteDocument("a.md");
  index.IndexDocument("a.md", "ring");
  EXPECT_EQ(index.DocumentCount(), 1U);
  EXPECT_EQ(index.DocumentFrequency("ring"), 1U);
}

TEST(IncrementalIndex, CompactDropsTombstonedPostings) {
  InvertedIndex index;
  index.IndexDocument("a.md", "chunk unique_to_a");
  index.IndexDocument("b.md", "chunk");
  index.DeleteDocument("a.md");

  const std::size_t before = index.Lookup("chunk")->size();
  ASSERT_EQ(before, 2U) << "the tombstoned posting is still physically present";

  index.Compact();
  ASSERT_NE(index.Lookup("chunk"), nullptr);
  EXPECT_EQ(index.Lookup("chunk")->size(), 1U) << "Compact must reclaim it";
  EXPECT_EQ(index.Lookup("unique_to_a"), nullptr) << "terms with no live document disappear";
  EXPECT_EQ(index.DocumentCount(), 1U);
  EXPECT_EQ(index.FileId(0), "b.md") << "surviving documents are renumbered densely";
}

TEST(IncrementalIndex, CompactPreservesSearchResults) {
  SearchEngine engine;
  engine.IndexDocument("keep.md", "consistent hashing ring");
  engine.IndexDocument("drop.md", "consistent hashing ring");
  engine.DeleteDocument("drop.md");

  const std::vector<SearchHit> before = engine.Search("hashing", 5);
  engine.Compact();
  const std::vector<SearchHit> after = engine.Search("hashing", 5);

  ASSERT_EQ(before.size(), 1U);
  ASSERT_EQ(after.size(), 1U);
  EXPECT_EQ(before.front().file_id, "keep.md");
  EXPECT_EQ(after.front().file_id, "keep.md");
}

TEST(IncrementalIndex, SearchStopsReturningDeletedDocuments) {
  SearchEngine engine;
  engine.IndexDocument("a.md", "consistent hashing");
  ASSERT_FALSE(engine.Search("hashing", 5).empty());

  EXPECT_TRUE(engine.DeleteDocument("a.md"));
  EXPECT_TRUE(engine.Search("hashing", 5).empty());
  EXPECT_EQ(engine.Stats().document_count, 0U);
}

TEST(IncrementalIndex, UpdatedContentIsSearchableAndOldContentIsNot) {
  SearchEngine engine;
  engine.IndexDocument("a.md", "write-ahead logging");
  engine.IndexDocument("a.md", "consistent hashing");

  EXPECT_TRUE(engine.Search("logging", 5).empty());
  ASSERT_FALSE(engine.Search("hashing", 5).empty());
  EXPECT_EQ(engine.Search("hashing", 5).front().file_id, "a.md");
}

}  // namespace atlas::search

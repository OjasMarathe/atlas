// Snippets, vocabulary decrement on delete, and automatic compaction — the Phase 3 leftovers.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "search/search_engine.h"
#include "search/tokenizer.h"

namespace atlas::search {

TEST(TokenizeWithOffsets, ReportsWhereEachTokenCameFrom) {
  const std::vector<LocatedToken> tokens = TokenizeWithOffsets("the write-ahead log");
  ASSERT_EQ(tokens.size(), 4U);
  EXPECT_EQ(tokens[0].text, "the");
  EXPECT_EQ(tokens[0].begin, 0U);
  EXPECT_EQ(tokens[0].end, 3U);
  EXPECT_EQ(tokens[1].text, "write");
  EXPECT_EQ(tokens[1].begin, 4U);
  EXPECT_EQ(tokens[1].end, 9U);
  EXPECT_EQ(tokens[3].text, "log");
  EXPECT_EQ(tokens[3].begin, 16U);
}

TEST(TokenizeWithOffsets, OffsetsIndexTheOriginalNotTheLowercasedCopy) {
  const std::vector<LocatedToken> tokens = TokenizeWithOffsets("  CHUNK  ");
  ASSERT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].text, "chunk");  // lowercased
  EXPECT_EQ(tokens[0].begin, 2U);      // but located in the original text
  EXPECT_EQ(tokens[0].end, 7U);
}

TEST(TokenizeWithOffsets, AgreesWithPlainTokenize) {
  const std::string text = "Consistent hashing: places chunks (on a ring).";
  const std::vector<std::string> plain = Tokenize(text);
  const std::vector<LocatedToken> located = TokenizeWithOffsets(text);
  ASSERT_EQ(plain.size(), located.size());
  for (std::size_t i = 0; i < plain.size(); ++i) EXPECT_EQ(plain[i], located[i].text);
}

TEST(Snippet, QuotesTheDocumentAroundTheMatch) {
  SearchEngine engine;
  engine.IndexDocument("wal.md",
                       "Some preamble that is long enough to be trimmed away from the front. "
                       "A write ahead log makes writes crash consistent. Trailing prose here.");
  const std::vector<SearchHit> hits = engine.Search("crash", 5);
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_NE(hits.front().snippet.find("crash"), std::string::npos);
}

TEST(Snippet, IsElidedAtBothEndsWhenTheDocumentIsLonger) {
  SearchEngine engine;
  const std::string filler(400, 'x');
  engine.IndexDocument("a.md", filler + " needle " + filler);
  const std::vector<SearchHit> hits = engine.Search("needle", 5);
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_EQ(hits.front().snippet.rfind("...", 0), 0U) << "should start with an ellipsis";
  EXPECT_NE(hits.front().snippet.find("needle"), std::string::npos);
}

TEST(Snippet, StaysOnOneLine) {
  SearchEngine engine;
  engine.IndexDocument("a.md", "first line\nsecond line with chunk in it\nthird line");
  const std::vector<SearchHit> hits = engine.Search("chunk", 5);
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_EQ(hits.front().snippet.find('\n'), std::string::npos);
}

// The snippet has to find the match through the same analysis the index used, or a stemmed
// query term would never line up with the surface word in the text.
TEST(Snippet, FindsTheMatchThroughStemming) {
  SearchEngine engine;
  engine.IndexDocument("a.md", "The system supports replication across nodes.");
  const std::vector<SearchHit> hits = engine.Search("replicate", 5);
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_NE(hits.front().snippet.find("replication"), std::string::npos);
}

TEST(Snippet, IsEmptyForAFilterOnlyQueryWithNothingToHighlight) {
  SearchEngine engine;
  engine.IndexDocument("a.md", "chunk replication", {{"author", "ojas"}});
  const std::vector<SearchHit> hits = engine.Search("author:ojas", 5);
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_TRUE(hits.front().snippet.empty()) << "no query term means nothing to center on";
}

TEST(VocabularyDecrement, DeletingRemovesWordsNoLiveDocumentContains) {
  SearchEngine engine;
  engine.IndexDocument("a.md", "kubernetes orchestration");
  engine.IndexDocument("b.md", "chunk replication");
  ASSERT_FALSE(engine.Suggest("kubern", 5).empty());

  ASSERT_TRUE(engine.DeleteDocument("a.md"));
  EXPECT_TRUE(engine.Suggest("kubern", 5).empty())
      << "a word from a deleted document must stop being suggested";
  EXPECT_FALSE(engine.Suggest("chun", 5).empty()) << "surviving words remain";
}

TEST(VocabularyDecrement, AWordSharedWithALiveDocumentSurvives) {
  SearchEngine engine;
  engine.IndexDocument("a.md", "replication");
  engine.IndexDocument("b.md", "replication");
  ASSERT_TRUE(engine.DeleteDocument("a.md"));
  EXPECT_FALSE(engine.Suggest("replic", 5).empty())
      << "b.md still contains it, so it must still be suggested";
}

TEST(VocabularyDecrement, SpellCorrectionStopsSuggestingDeletedWords) {
  SearchEngine engine;
  engine.IndexDocument("a.md", "kubernetes");
  engine.IndexDocument("b.md", "chunk");
  ASSERT_FALSE(engine.DidYouMean("kubernets").empty());

  ASSERT_TRUE(engine.DeleteDocument("a.md"));
  EXPECT_TRUE(engine.DidYouMean("kubernets").empty());
}

TEST(AutoCompaction, TriggersOnceTombstonesPassTheThreshold) {
  SearchEngine engine;
  for (int i = 0; i < 10; ++i) engine.IndexDocument("doc" + std::to_string(i), "chunk");
  ASSERT_EQ(engine.index().DeletedDocumentCount(), 0U);

  // Three of ten tombstoned is at the threshold, not past it.
  for (int i = 0; i < 3; ++i) engine.DeleteDocument("doc" + std::to_string(i));
  EXPECT_EQ(engine.index().DeletedDocumentCount(), 3U);

  // The fourth pushes it over, and compaction reclaims everything.
  engine.DeleteDocument("doc3");
  EXPECT_EQ(engine.index().DeletedDocumentCount(), 0U) << "auto-compaction should have run";
  EXPECT_EQ(engine.index().DocumentCount(), 6U);
}

TEST(AutoCompaction, LeavesSearchResultsCorrect) {
  SearchEngine engine;
  for (int i = 0; i < 10; ++i) {
    engine.IndexDocument("doc" + std::to_string(i), "consistent hashing ring");
  }
  for (int i = 0; i < 5; ++i) engine.DeleteDocument("doc" + std::to_string(i));

  const std::vector<SearchHit> hits = engine.Search("hashing", 20);
  EXPECT_EQ(hits.size(), 5U);
  for (const SearchHit& hit : hits) {
    EXPECT_EQ(hit.file_id.rfind("doc", 0), 0U);
    EXPECT_GE(hit.file_id, "doc5") << "only the surviving half should match";
  }
}

}  // namespace atlas::search

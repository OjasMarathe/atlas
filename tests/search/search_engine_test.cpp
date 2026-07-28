#include "search/search_engine.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace atlas::search {
namespace {

// A tiny corpus with deliberately different topics, so "the right documents on top" is checkable.
SearchEngine BuildEngine() {
  SearchEngine engine;
  engine.IndexDocument("hashing.md",
                       "Consistent hashing places chunks on nodes using a ring of virtual "
                       "nodes so that joins and leaves migrate little data.");
  engine.IndexDocument("replication.md",
                       "Each chunk is replicated onto three distinct nodes. Replication keeps "
                       "the cluster available when a node dies.");
  engine.IndexDocument("bm25.md",
                       "BM25 ranks documents by term frequency saturation and document length "
                       "normalization. Ranking is lexical, not semantic.");
  engine.IndexDocument("wal.md",
                       "A write-ahead log makes writes crash consistent: append to the log, "
                       "fsync, then apply.");
  return engine;
}

std::vector<std::string> FileIds(const std::vector<SearchHit>& hits) {
  std::vector<std::string> ids;
  ids.reserve(hits.size());
  for (const SearchHit& hit : hits) ids.push_back(hit.file_id);
  return ids;
}

bool Contains(const std::vector<SearchHit>& hits, std::string_view file_id) {
  for (const SearchHit& hit : hits) {
    if (hit.file_id == file_id) return true;
  }
  return false;
}

}  // namespace

TEST(SearchEngine, RanksTheMostRelevantDocumentFirst) {
  const SearchEngine engine = BuildEngine();
  const std::vector<SearchHit> hits = engine.Search("consistent hashing ring", 5);
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(hits.front().file_id, "hashing.md");
}

TEST(SearchEngine, MatchesAcrossInflectionsViaStemming) {
  const SearchEngine engine = BuildEngine();
  // The document says "replicated"/"Replication"; the query says "replicate".
  const std::vector<SearchHit> hits = engine.Search("replicate", 5);
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(hits.front().file_id, "replication.md");
}

TEST(SearchEngine, ScoresAreDescendingAndPositive) {
  const SearchEngine engine = BuildEngine();
  const std::vector<SearchHit> hits = engine.Search("chunk nodes", 5);
  ASSERT_GE(hits.size(), 2U);
  for (std::size_t i = 1; i < hits.size(); ++i) {
    EXPECT_GE(hits[i - 1].score, hits[i].score);
  }
  EXPECT_GT(hits.front().score, 0.0);
}

TEST(SearchEngine, ImplicitOperatorIsOrSoPartialMatchesStillRank) {
  const SearchEngine engine = BuildEngine();
  // No document contains both "hashing" and "fsync".
  const std::vector<SearchHit> hits = engine.Search("hashing fsync", 5);
  EXPECT_EQ(hits.size(), 2U);
  EXPECT_TRUE(Contains(hits, "hashing.md"));
  EXPECT_TRUE(Contains(hits, "wal.md"));
}

TEST(SearchEngine, AndRequiresEveryTerm) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.Search("hashing AND fsync", 5).empty());

  const std::vector<SearchHit> hits = engine.Search("chunk AND replicated", 5);
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_EQ(hits.front().file_id, "replication.md");
}

TEST(SearchEngine, NotExcludesMatchingDocuments) {
  const SearchEngine engine = BuildEngine();
  const std::vector<SearchHit> with_both = engine.Search("nodes", 5);
  ASSERT_EQ(with_both.size(), 2U);  // hashing.md and replication.md

  const std::vector<SearchHit> excluded = engine.Search("nodes AND NOT replication", 5);
  ASSERT_EQ(excluded.size(), 1U);
  EXPECT_EQ(excluded.front().file_id, "hashing.md");
}

TEST(SearchEngine, ParenthesesGroupClauses) {
  const SearchEngine engine = BuildEngine();
  const std::vector<SearchHit> hits = engine.Search("(fsync OR ranks) AND NOT hashing", 5);
  const std::vector<std::string> ids = FileIds(hits);
  EXPECT_EQ(ids.size(), 2U);
  EXPECT_TRUE(Contains(hits, "wal.md"));
  EXPECT_TRUE(Contains(hits, "bm25.md"));
}

TEST(SearchEngine, TopKLimitsResults) {
  const SearchEngine engine = BuildEngine();
  EXPECT_LE(engine.Search("nodes chunk log ranking", 1).size(), 1U);
}

TEST(SearchEngine, UnknownTermsAndStopWordQueriesReturnNothing) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.Search("kubernetes", 5).empty());
  EXPECT_TRUE(engine.Search("the and of", 5).empty());
  EXPECT_TRUE(engine.Search("", 5).empty());
}

TEST(SearchEngine, MalformedQueryReportsAnError) {
  const SearchEngine engine = BuildEngine();
  std::string error;
  const std::vector<SearchHit> hits = engine.Search("(chunk AND", 5, &error);
  EXPECT_TRUE(hits.empty());
  EXPECT_FALSE(error.empty());
}

TEST(SearchEngine, StatsReflectTheIndexedCorpus) {
  const SearchEngine engine = BuildEngine();
  const ShardStatistics stats = engine.Stats();
  EXPECT_EQ(stats.document_count, 4U);
  EXPECT_GT(stats.unique_terms, 0U);
  EXPECT_GT(stats.average_document_length, 0.0);
}

TEST(SearchEngine, EmptyShardAnswersWithoutCrashing) {
  const SearchEngine engine;
  EXPECT_TRUE(engine.Search("anything", 5).empty());
  EXPECT_EQ(engine.Stats().document_count, 0U);
}

}  // namespace atlas::search

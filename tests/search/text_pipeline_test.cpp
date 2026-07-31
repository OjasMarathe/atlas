#include "search/text_pipeline.h"

#include <gtest/gtest.h>

#include <vector>

#include "search/stopwords.h"

namespace atlas::search {

TEST(StopWords, RecognizesFunctionWordsOnly) {
  EXPECT_TRUE(IsStopWord("the"));
  EXPECT_TRUE(IsStopWord("does"));
  EXPECT_TRUE(IsStopWord("a"));
  EXPECT_FALSE(IsStopWord("filesystem"));
  EXPECT_FALSE(IsStopWord("journal"));
}

TEST(Analyze, RunsTheFullPipeline) {
  // "how"/"does"/"a" are stop words; "filesystem" survives; "writes" stems to "write".
  // Each Term carries {stem, surface form, position}.
  EXPECT_EQ(Analyze("How does a filesystem journal writes?"),
            (std::vector<Term>{
                {"filesystem", "filesystem", 3}, {"journal", "journal", 4}, {"write", "writes", 5}}));
}

// Autocomplete and spell correction suggest real words, so the analyzer keeps the surface form
// alongside the stem it indexes by.
TEST(Analyze, KeepsTheSurfaceFormBesideTheStem) {
  const std::vector<Term> terms = Analyze("replicating");
  ASSERT_EQ(terms.size(), 1U);
  EXPECT_EQ(terms[0].text, "replic");
  EXPECT_EQ(terms[0].surface, "replicating");
}

// Positions come from the pre-filter token stream, so the gap left by a dropped stop word is
// still visible — phrase search in Phase 3b depends on this.
TEST(Analyze, PositionsIndexTheOriginalTokenStream) {
  const std::vector<Term> terms = Analyze("chunks are replicated across the ring");
  ASSERT_EQ(terms.size(), 4U);
  EXPECT_EQ(terms[0].position, 0U);  // chunks
  EXPECT_EQ(terms[1].position, 2U);  // replicated  ("are" dropped at 1)
  EXPECT_EQ(terms[2].position, 3U);  // across
  EXPECT_EQ(terms[3].position, 5U);  // ring        ("the" dropped at 4)
}

// The pipeline's whole purpose: a document saying "replicates" is findable by a query saying
// "replication", because both sides run through the same tokenize/stem path.
TEST(Analyze, QueryAndDocumentFormsMeetAtTheSameTerm) {
  const std::vector<Term> document = Analyze("The coordinator replicates each chunk.");
  const std::vector<Term> query = Analyze("replication");
  ASSERT_EQ(document.size(), 3U);
  ASSERT_EQ(query.size(), 1U);
  EXPECT_EQ(document[1].text, query[0].text);
}

TEST(Analyze, EmptyAndStopWordOnlyTextYieldNoTerms) {
  EXPECT_TRUE(Analyze("").empty());
  EXPECT_TRUE(Analyze("the and of a to").empty());
}

}  // namespace atlas::search

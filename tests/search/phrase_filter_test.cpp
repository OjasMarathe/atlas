// Phase 3b query features: quoted phrase search, field filters, autocomplete, spell correction.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "search/search_engine.h"

namespace atlas::search {
namespace {

SearchEngine BuildEngine() {
  SearchEngine engine;
  engine.IndexDocument("wal.md", "A write ahead log makes writes crash consistent.",
                       {{"author", "ojas"}, {"type", "note"}, {"lang", "en"}});
  engine.IndexDocument("hashing.md", "Consistent hashing places chunks on a ring of nodes.",
                       {{"author", "harshal"}, {"type", "note"}, {"lang", "en"}});
  // "log" and "write" both appear, but never adjacent and in the other order.
  engine.IndexDocument("ordering.md", "The log is appended, then we write.",
                       {{"author", "ojas"}, {"type", "draft"}, {"lang", "en"}});
  engine.IndexDocument("gap.md", "chunks of the ring", {{"author", "harshal"}, {"type", "draft"}});
  return engine;
}

std::vector<std::string> FileIds(const std::vector<SearchHit>& hits) {
  std::vector<std::string> ids;
  ids.reserve(hits.size());
  for (const SearchHit& hit : hits) ids.push_back(hit.file_id);
  return ids;
}

}  // namespace

TEST(PhraseSearch, MatchesOnlyAdjacentTermsInOrder) {
  const SearchEngine engine = BuildEngine();
  const std::vector<SearchHit> hits = engine.Search("\"write ahead log\"", 5);
  EXPECT_EQ(FileIds(hits), (std::vector<std::string>{"wal.md"}));
}

TEST(PhraseSearch, RejectsTheSameWordsInTheWrongOrder) {
  const SearchEngine engine = BuildEngine();
  // ordering.md has both "log" and "write", but as "log ... write".
  EXPECT_TRUE(engine.Search("\"write log\"", 5).empty());
}

// Positions come from the pre-filter token stream, so a dropped stop word still occupies a slot.
// Without that, "chunks ring" would wrongly match "chunks of the ring".
TEST(PhraseSearch, DroppedStopWordsStillOccupyTheirSlot) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.Search("\"chunks ring\"", 5).empty())
      << "the two words are three slots apart, not adjacent";
}

// Consequence of the same design: stop words are absent from the index, so a phrase can only
// check *how wide* the gap is, not which words filled it. "chunks of the ring" therefore also
// matches "chunks on a ring" — both are chunks + two dropped words + ring. Lucene behaves the
// same way with a stop filter; documented in concepts/boolean-phrase-search.md.
TEST(PhraseSearch, StopWordsInAPhraseActAsPositionalWildcards) {
  const SearchEngine engine = BuildEngine();
  std::vector<std::string> ids = FileIds(engine.Search("\"chunks of the ring\"", 5));
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<std::string>{"gap.md", "hashing.md"}));
}

TEST(PhraseSearch, DiffersFromPlainConjunction) {
  const SearchEngine engine = BuildEngine();
  // As an AND both documents qualify; as a phrase only the adjacent one does.
  EXPECT_EQ(engine.Search("write AND log", 5).size(), 2U);
  EXPECT_EQ(engine.Search("\"write ahead log\"", 5).size(), 1U);
}

TEST(PhraseSearch, SingleWordPhraseBehavesLikeATerm) {
  const SearchEngine engine = BuildEngine();
  EXPECT_EQ(FileIds(engine.Search("\"hashing\"", 5)), (std::vector<std::string>{"hashing.md"}));
}

TEST(PhraseSearch, UnterminatedQuoteRunsToTheEndInsteadOfFailing) {
  const SearchEngine engine = BuildEngine();
  std::string error;
  const std::vector<SearchHit> hits = engine.Search("\"write ahead log", 5, &error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(FileIds(hits), (std::vector<std::string>{"wal.md"}));
}

TEST(PhraseSearch, PhraseCombinesWithBooleanOperators) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.Search("\"write ahead log\" AND hashing", 5).empty());
  EXPECT_EQ(engine.Search("\"write ahead log\" OR hashing", 5).size(), 2U);
}

TEST(Filters, RestrictResultsToMatchingDocuments) {
  const SearchEngine engine = BuildEngine();
  const std::vector<std::string> ojas = FileIds(engine.Search("author:ojas", 10));
  EXPECT_EQ(ojas.size(), 2U);

  const std::vector<std::string> drafts = FileIds(engine.Search("type:draft", 10));
  EXPECT_EQ(drafts.size(), 2U);
}

TEST(Filters, CombineWithTermsViaAnd) {
  const SearchEngine engine = BuildEngine();
  EXPECT_EQ(FileIds(engine.Search("log AND author:ojas AND type:note", 10)),
            (std::vector<std::string>{"wal.md"}));
}

TEST(Filters, MatchValuesExactlyAndAreNotAnalyzed) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.Search("author:oja", 10).empty()) << "prefixes must not match";
  EXPECT_TRUE(engine.Search("author:OJAS", 10).empty()) << "values are matched verbatim";
}

TEST(Filters, UnknownFieldOrValueYieldsNothing) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.Search("author:nobody", 10).empty());
  EXPECT_TRUE(engine.Search("nosuchfield:note", 10).empty());
}

// A filter narrows the candidate set but must not contribute to the relevance score.
TEST(Filters, DoNotAffectRanking) {
  const SearchEngine engine = BuildEngine();
  const std::vector<SearchHit> plain = engine.Search("hashing", 5);
  const std::vector<SearchHit> filtered = engine.Search("hashing AND author:harshal", 5);
  ASSERT_EQ(plain.size(), 1U);
  ASSERT_EQ(filtered.size(), 1U);
  EXPECT_DOUBLE_EQ(plain.front().score, filtered.front().score);
}

TEST(Autocomplete, CompletesIndexedWordsFromAPrefix) {
  const SearchEngine engine = BuildEngine();
  const std::vector<Completion> completions = engine.Suggest("wri", 5);
  ASSERT_FALSE(completions.empty());
  for (const Completion& completion : completions) {
    EXPECT_EQ(completion.word.rfind("wri", 0), 0U);
  }
}

// The index is keyed by stems, but a user typing "consist" expects a real word back.
TEST(Autocomplete, SuggestsSurfaceFormsNotStems) {
  const SearchEngine engine = BuildEngine();
  const std::vector<Completion> completions = engine.Suggest("consist", 5);
  ASSERT_FALSE(completions.empty());
  EXPECT_EQ(completions.front().word, "consistent");
}

TEST(Autocomplete, UnknownPrefixYieldsNothing) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.Suggest("kubernet", 5).empty());
  EXPECT_TRUE(engine.Suggest("", 5).empty());
}

TEST(SpellCorrection, SuggestsNearbyIndexedWords) {
  const SearchEngine engine = BuildEngine();
  const std::vector<Suggestion> suggestions = engine.DidYouMean("hashng");
  ASSERT_FALSE(suggestions.empty());
  EXPECT_EQ(suggestions.front().word, "hashing");
}

TEST(SpellCorrection, StaysSilentForCorrectlySpelledWords) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.DidYouMean("hashing").empty()) << "nothing to correct";
}

TEST(SpellCorrection, GivesUpOnWordsThatAreTooFarOff) {
  const SearchEngine engine = BuildEngine();
  EXPECT_TRUE(engine.DidYouMean("kubernetes").empty());
}

}  // namespace atlas::search

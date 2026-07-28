#include "common/query/parser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace atlas::query {
namespace {

// The parser is analyzer-agnostic; these tests inject a trivial analyzer so they exercise
// grammar rather than stemming. "the" stands in for a stop word.
Options WithTestAnalyzer(Node::Kind implicit = Node::Kind::Or) {
  Options options;
  options.implicit_operator = implicit;
  options.analyze_term = [](std::string_view word) -> std::string {
    if (word == "the" || word == "a") return {};  // dropped, like a stop word
    return std::string(word);
  };
  return options;
}

}  // namespace

TEST(Parser, SingleTermBecomesATermNode) {
  const Result result = Parse("chunk", WithTestAnalyzer());
  ASSERT_TRUE(result.ok()) << result.error;
  ASSERT_NE(result.root, nullptr);
  EXPECT_EQ(result.root->kind, Node::Kind::Term);
  EXPECT_EQ(result.root->term, "chunk");
}

TEST(Parser, AdjacentTermsUseTheImplicitOperator) {
  const Result or_result = Parse("chunk replication", WithTestAnalyzer(Node::Kind::Or));
  ASSERT_TRUE(or_result.ok());
  ASSERT_NE(or_result.root, nullptr);
  EXPECT_EQ(or_result.root->kind, Node::Kind::Or);
  EXPECT_EQ(or_result.root->children.size(), 2U);

  const Result and_result = Parse("chunk replication", WithTestAnalyzer(Node::Kind::And));
  ASSERT_TRUE(and_result.ok());
  ASSERT_NE(and_result.root, nullptr);
  EXPECT_EQ(and_result.root->kind, Node::Kind::And);
}

TEST(Parser, ExplicitOperatorsAreRecognized) {
  const Result and_result = Parse("chunk AND replication", WithTestAnalyzer());
  ASSERT_TRUE(and_result.ok());
  EXPECT_EQ(and_result.root->kind, Node::Kind::And);

  const Result not_result = Parse("NOT chunk", WithTestAnalyzer());
  ASSERT_TRUE(not_result.ok());
  ASSERT_NE(not_result.root, nullptr);
  EXPECT_EQ(not_result.root->kind, Node::Kind::Not);
  EXPECT_EQ(not_result.root->children.size(), 1U);
}

// AND must bind tighter than OR: "a OR b AND c" is "a OR (b AND c)".
TEST(Parser, AndBindsTighterThanOr) {
  const Result result = Parse("chunk OR ring AND hash", WithTestAnalyzer());
  ASSERT_TRUE(result.ok());
  ASSERT_NE(result.root, nullptr);
  EXPECT_EQ(result.root->kind, Node::Kind::Or);
  ASSERT_EQ(result.root->children.size(), 2U);
  EXPECT_EQ(result.root->children[0]->kind, Node::Kind::Term);
  EXPECT_EQ(result.root->children[1]->kind, Node::Kind::And);
}

TEST(Parser, ParenthesesOverridePrecedence) {
  const Result result = Parse("(chunk OR ring) AND hash", WithTestAnalyzer());
  ASSERT_TRUE(result.ok());
  ASSERT_NE(result.root, nullptr);
  EXPECT_EQ(result.root->kind, Node::Kind::And);
  ASSERT_EQ(result.root->children.size(), 2U);
  EXPECT_EQ(result.root->children[0]->kind, Node::Kind::Or);
}

// Operators are uppercase keywords, so ordinary prose words are terms — critical because "not"
// is itself a stop word (see concepts/tokenization-stemming.md).
TEST(Parser, LowercaseOperatorWordsAreOrdinaryTerms) {
  const Result result = Parse("chunk and ring", WithTestAnalyzer());
  ASSERT_TRUE(result.ok());
  ASSERT_NE(result.root, nullptr);
  EXPECT_EQ(result.root->kind, Node::Kind::Or);  // implicit operator, not an explicit AND
  ASSERT_EQ(result.root->children.size(), 3U);   // chunk, and, ring
}

TEST(Parser, TermsThatAnalyzeAwayAreDropped) {
  const Result result = Parse("the chunk", WithTestAnalyzer());
  ASSERT_TRUE(result.ok());
  ASSERT_NE(result.root, nullptr);
  EXPECT_EQ(result.root->kind, Node::Kind::Term) << "single survivor needs no operator wrapper";
  EXPECT_EQ(result.root->term, "chunk");
}

TEST(Parser, QueryOfOnlyStopWordsYieldsNoTreeButNoError) {
  const Result result = Parse("the a the", WithTestAnalyzer());
  EXPECT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.root, nullptr);
}

TEST(Parser, EmptyQueryIsNotAnError) {
  const Result result = Parse("   ", WithTestAnalyzer());
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.root, nullptr);
}

TEST(Parser, MalformedQueriesReportErrors) {
  EXPECT_FALSE(Parse("(chunk", WithTestAnalyzer()).ok());
  EXPECT_FALSE(Parse("chunk)", WithTestAnalyzer()).ok());
  EXPECT_FALSE(Parse("AND chunk", WithTestAnalyzer()).ok());
  EXPECT_FALSE(Parse("chunk AND", WithTestAnalyzer()).ok());
  EXPECT_FALSE(Parse("NOT", WithTestAnalyzer()).ok());
}

TEST(PositiveTerms, CollectsScorableTermsAndSkipsNegatedOnes) {
  const Result result = Parse("chunk AND ring NOT hash", WithTestAnalyzer());
  ASSERT_TRUE(result.ok()) << result.error;
  const std::vector<std::string> terms = PositiveTerms(result.root.get());
  EXPECT_EQ(terms, (std::vector<std::string>{"chunk", "ring"}));
}

TEST(PositiveTerms, DeduplicatesWhilePreservingOrder) {
  const Result result = Parse("chunk OR ring OR chunk", WithTestAnalyzer());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(PositiveTerms(result.root.get()), (std::vector<std::string>{"chunk", "ring"}));
}

TEST(PositiveTerms, HandlesNullRoot) { EXPECT_TRUE(PositiveTerms(nullptr).empty()); }

}  // namespace atlas::query

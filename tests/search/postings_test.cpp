#include "search/postings.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <numeric>
#include <vector>

namespace atlas::search {

TEST(Postings, IntersectKeepsOnlyCommonDocs) {
  const std::vector<DocId> a{1, 3, 5, 7, 9};
  const std::vector<DocId> b{3, 4, 5, 9, 12};
  EXPECT_EQ(Intersect(a, b), (std::vector<DocId>{3, 5, 9}));
}

TEST(Postings, IntersectWithEmptyOrDisjointIsEmpty) {
  const std::vector<DocId> a{1, 2, 3};
  EXPECT_TRUE(Intersect(a, {}).empty());
  EXPECT_TRUE(Intersect({}, a).empty());
  EXPECT_TRUE(Intersect(a, {7, 8, 9}).empty());
}

TEST(Postings, UnionMergesAndDeduplicates) {
  EXPECT_EQ(Union({1, 3, 5}, {3, 4, 9}), (std::vector<DocId>{1, 3, 4, 5, 9}));
  EXPECT_EQ(Union({}, {2, 4}), (std::vector<DocId>{2, 4}));
}

TEST(Postings, DifferenceRemovesTheSecondList) {
  EXPECT_EQ(Difference({1, 2, 3, 4}, {2, 4}), (std::vector<DocId>{1, 3}));
  EXPECT_EQ(Difference({1, 2}, {}), (std::vector<DocId>{1, 2}));
  EXPECT_TRUE(Difference({1, 2}, {1, 2}).empty());
}

TEST(Postings, ResultsStaySorted) {
  const std::vector<DocId> a{2, 4, 6, 8, 10, 12};
  const std::vector<DocId> b{1, 4, 5, 8, 11, 12};
  for (const auto& result : {Intersect(a, b), Union(a, b), Difference(a, b)}) {
    for (std::size_t i = 1; i < result.size(); ++i) EXPECT_LT(result[i - 1], result[i]);
  }
}

TEST(SkipList, AdvanceFindsFirstDocAtOrAfterTarget) {
  std::vector<DocId> docs(100);
  // 0, 2, 4, ... 198 — only even doc ids, so odd targets must land on the next even one.
  for (std::size_t i = 0; i < docs.size(); ++i) docs[i] = static_cast<DocId>(i * 2);
  const SkipList skips(docs);

  EXPECT_EQ(skips.Advance(docs, 0, 0), 0U);
  EXPECT_EQ(skips.Advance(docs, 0, 1), 1U);             // first doc >= 1 is docs[1] == 2
  EXPECT_EQ(skips.Advance(docs, 0, 100), 50U);          // docs[50] == 100
  EXPECT_EQ(skips.Advance(docs, 0, 197), 99U);          // docs[99] == 198
  EXPECT_EQ(skips.Advance(docs, 0, 199), docs.size());  // past the end
}

TEST(SkipList, AdvanceNeverGoesBackwards) {
  std::vector<DocId> docs(64);
  std::iota(docs.begin(), docs.end(), 0);
  const SkipList skips(docs);
  // Target is behind `from`, so the answer must still be `from` — a merge must never rewind.
  EXPECT_EQ(skips.Advance(docs, 40, 5), 40U);
}

TEST(SkipList, HandlesEmptyAndSingletonLists) {
  const std::vector<DocId> empty;
  const SkipList empty_skips(empty);
  EXPECT_EQ(empty_skips.Advance(empty, 0, 3), 0U);

  const std::vector<DocId> single{42};
  const SkipList single_skips(single);
  EXPECT_EQ(single_skips.Advance(single, 0, 42), 0U);
  EXPECT_EQ(single_skips.Advance(single, 0, 43), 1U);
}

// Skip pointers are an optimization, so they must not change the answer. Cross-check the
// skip-accelerated intersection against a plain linear merge over a large, lopsided pair of
// lists — the case skipping is supposed to help.
TEST(SkipList, SkipAcceleratedIntersectMatchesLinearMerge) {
  std::vector<DocId> dense(2000);
  for (std::size_t i = 0; i < dense.size(); ++i) dense[i] = static_cast<DocId>(i);
  const std::vector<DocId> sparse{7, 500, 501, 1999, 5000};

  std::vector<DocId> expected;
  std::set_intersection(dense.begin(), dense.end(), sparse.begin(), sparse.end(),
                        std::back_inserter(expected));
  EXPECT_EQ(Intersect(dense, sparse), expected);
  EXPECT_EQ(Intersect(sparse, dense), expected);
}

}  // namespace atlas::search

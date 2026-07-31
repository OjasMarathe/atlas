#include "search/posting_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "search/inverted_index.h"

namespace atlas::search {

TEST(Varint, RoundTripsBoundaryValues) {
  for (const std::uint64_t value :
       {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{127}, std::uint64_t{128},
        std::uint64_t{16383}, std::uint64_t{16384}, std::uint64_t{0xFFFFFFFF}, ~std::uint64_t{0}}) {
    std::string encoded;
    PutVarint(value, &encoded);
    std::size_t offset = 0;
    std::uint64_t decoded = 0;
    ASSERT_TRUE(GetVarint(encoded, &offset, &decoded)) << value;
    EXPECT_EQ(decoded, value);
    EXPECT_EQ(offset, encoded.size());
  }
}

// The whole point: small numbers must cost one byte, not four.
TEST(Varint, SmallValuesUseOneByte) {
  std::string encoded;
  PutVarint(127, &encoded);
  EXPECT_EQ(encoded.size(), 1U);
  encoded.clear();
  PutVarint(128, &encoded);
  EXPECT_EQ(encoded.size(), 2U);
}

TEST(Varint, RejectsTruncatedInput) {
  std::string encoded;
  PutVarint(300, &encoded);
  encoded.resize(1);  // drop the continuation byte
  std::size_t offset = 0;
  std::uint64_t decoded = 0;
  EXPECT_FALSE(GetVarint(encoded, &offset, &decoded));
}

TEST(PostingCodec, RoundTripsAPostingList) {
  PostingList original{
      Posting{3, 2, {5, 40}},
      Posting{9, 1, {0}},
      Posting{1000, 3, {1, 2, 900}},
  };
  const std::string encoded = EncodePostingList(original);

  PostingList decoded;
  ASSERT_TRUE(DecodePostingList(encoded, &decoded));
  ASSERT_EQ(decoded.size(), original.size());
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(decoded[i].doc_id, original[i].doc_id);
    EXPECT_EQ(decoded[i].term_frequency, original[i].term_frequency);
    EXPECT_EQ(decoded[i].positions, original[i].positions);
  }
}

TEST(PostingCodec, RoundTripsAnEmptyList) {
  const std::string encoded = EncodePostingList({});
  PostingList decoded;
  ASSERT_TRUE(DecodePostingList(encoded, &decoded));
  EXPECT_TRUE(decoded.empty());
}

TEST(PostingCodec, RejectsTruncatedInput) {
  const PostingList original{Posting{3, 2, {5, 40}}, Posting{9, 1, {0}}};
  std::string encoded = EncodePostingList(original);
  encoded.resize(encoded.size() / 2);
  PostingList decoded;
  EXPECT_FALSE(DecodePostingList(encoded, &decoded));
}

namespace {

// What the same postings would cost with fixed-width fields: doc_id, term frequency and the
// position count, four bytes each. The fair baseline to judge the codec against.
std::size_t UncompressedSize(const PostingList& postings) {
  std::size_t total = 0;
  for (const Posting& posting : postings) {
    total += 3 * sizeof(std::uint32_t) + posting.positions.size() * sizeof(std::uint32_t);
  }
  return total;
}

}  // namespace

// Delta encoding turns a dense ascending run into a run of 1s, each costing a single byte.
TEST(PostingCodec, DeltaEncodingShrinksDenseRuns) {
  PostingList dense;
  for (DocId id = 0; id < 1000; ++id) dense.push_back(Posting{id, 1, {}});
  const std::string encoded = EncodePostingList(dense);
  EXPECT_LT(encoded.size(), UncompressedSize(dense) / 2)
      << "encoded " << encoded.size() << " vs fixed-width " << UncompressedSize(dense);
}

// Large but tightly-spaced ids are the case delta encoding is really for: the ids themselves
// need four bytes each, the gaps between them need one.
TEST(PostingCodec, LargeIdsWithSmallGapsStayCheap) {
  PostingList sparse;
  for (DocId id = 1'000'000; id < 1'000'500; ++id) sparse.push_back(Posting{id, 1, {}});
  const std::string encoded = EncodePostingList(sparse);
  EXPECT_LT(encoded.size(), UncompressedSize(sparse) / 2);
  // Each doc id would need 4 bytes raw; the gap of 1 needs one byte.
  EXPECT_LT(encoded.size(), sparse.size() * 4);
}

TEST(InvertedIndexSerialization, RoundTripsAnIndex) {
  InvertedIndex original;
  original.IndexDocument("a.md", "chunk replication ring", {{"author", "ojas"}});
  original.IndexDocument("b.md", "chunk chunk hashing");
  original.DeleteDocument("b.md");
  original.IndexDocument("c.md", "consistent hashing ring");

  const std::string bytes = original.Serialize();
  InvertedIndex loaded;
  ASSERT_TRUE(loaded.Load(bytes));

  EXPECT_EQ(loaded.DocumentCount(), original.DocumentCount());
  EXPECT_EQ(loaded.UniqueTerms(), original.UniqueTerms());
  EXPECT_DOUBLE_EQ(loaded.AverageDocumentLength(), original.AverageDocumentLength());
  EXPECT_EQ(loaded.DocumentFrequency("chunk"), original.DocumentFrequency("chunk"));
  EXPECT_EQ(loaded.AllDocuments(), original.AllDocuments());
  EXPECT_EQ(loaded.DocumentsWithField("author", "ojas"),
            original.DocumentsWithField("author", "ojas"));

  const PostingList* postings = loaded.Lookup("ring");
  ASSERT_NE(postings, nullptr);
  EXPECT_EQ(postings->size(), 2U);
}

TEST(InvertedIndexSerialization, RejectsGarbage) {
  InvertedIndex index;
  index.IndexDocument("a.md", "chunk");
  const std::string good = index.Serialize();

  InvertedIndex loaded;
  EXPECT_FALSE(loaded.Load(good.substr(0, good.size() / 2)));
  // A failed load must leave the target untouched rather than half-populated.
  EXPECT_EQ(loaded.DocumentCount(), 0U);
}

}  // namespace atlas::search

#include "search/stemmer.h"

#include <gtest/gtest.h>

#include <string>

namespace atlas::search {

// Cases from Porter (1980)'s own rule examples, traced through all five phases.
TEST(Stemmer, Step1aPlurals) {
  EXPECT_EQ(Stem("caresses"), "caress");
  EXPECT_EQ(Stem("ponies"), "poni");
  EXPECT_EQ(Stem("ties"), "ti");
  EXPECT_EQ(Stem("caress"), "caress");
  EXPECT_EQ(Stem("cats"), "cat");
}

TEST(Stemmer, Step1bPastAndProgressive) {
  EXPECT_EQ(Stem("feed"), "feed");
  EXPECT_EQ(Stem("plastered"), "plaster");
  EXPECT_EQ(Stem("bled"), "bled");
  EXPECT_EQ(Stem("motoring"), "motor");
  EXPECT_EQ(Stem("sing"), "sing");
}

TEST(Stemmer, Step1bCleanupRestoresStems) {
  EXPECT_EQ(Stem("hopping"), "hop");
  EXPECT_EQ(Stem("tanned"), "tan");
  EXPECT_EQ(Stem("falling"), "fall");
  EXPECT_EQ(Stem("hissing"), "hiss");
  EXPECT_EQ(Stem("fizzed"), "fizz");
  EXPECT_EQ(Stem("filing"), "file");
  EXPECT_EQ(Stem("sized"), "size");
}

TEST(Stemmer, Step1cTerminalY) {
  EXPECT_EQ(Stem("happy"), "happi");
  EXPECT_EQ(Stem("sky"), "sky");
}

TEST(Stemmer, Step5DoubleConsonantAndTrailingE) {
  EXPECT_EQ(Stem("controll"), "control");
  EXPECT_EQ(Stem("roll"), "roll");
  EXPECT_EQ(Stem("rate"), "rate");
}

TEST(Stemmer, ShortWordsUnchanged) {
  EXPECT_EQ(Stem(""), "");
  EXPECT_EQ(Stem("a"), "a");
  EXPECT_EQ(Stem("is"), "is");
  EXPECT_EQ(Stem("as"), "as");
}

// The property that actually matters for retrieval: surface variants collapse to one term, so a
// query for one form matches documents using another.
TEST(Stemmer, ConflatesInflectionsOfTheSameWord) {
  const std::string connect = Stem("connect");
  EXPECT_EQ(Stem("connected"), connect);
  EXPECT_EQ(Stem("connecting"), connect);
  EXPECT_EQ(Stem("connection"), connect);
  EXPECT_EQ(Stem("connections"), connect);

  const std::string replicate = Stem("replicate");
  EXPECT_EQ(Stem("replicated"), replicate);
  EXPECT_EQ(Stem("replicating"), replicate);
}

TEST(Stemmer, DistinctWordsDoNotCollide) {
  EXPECT_NE(Stem("replication"), Stem("reply"));
  EXPECT_NE(Stem("chunking"), Stem("checksum"));
}

}  // namespace atlas::search

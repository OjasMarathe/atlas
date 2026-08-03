// RocksDB persistence for the inverted index — the storage half of ADR-0007.

#include "search/index_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "search/search_engine.h"

namespace atlas::search {
namespace {

namespace fs = std::filesystem;

// A unique temp directory per test, removed on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_((fs::temp_directory_path() / ("atlas_index_store_" + name)).string()) {
    fs::remove_all(path_);
  }
  ~TempDir() { fs::remove_all(path_); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

InvertedIndex BuildIndex() {
  InvertedIndex index;
  index.IndexDocument("wal.md", "A write ahead log makes writes crash consistent.",
                      {{"author", "ojas"}, {"type", "note"}});
  index.IndexDocument("hashing.md", "Consistent hashing places chunks on a ring of nodes.",
                      {{"author", "harshal"}});
  index.IndexDocument("stale.md", "This document will be deleted.");
  index.DeleteDocument("stale.md");
  return index;
}

}  // namespace

TEST(IndexStore, OpensAndReportsOk) {
  const TempDir dir("open");
  const IndexStore store(dir.path());
  EXPECT_TRUE(store.ok());
}

TEST(IndexStore, RoundTripsAnIndexThroughDisk) {
  const TempDir dir("roundtrip");
  const InvertedIndex original = BuildIndex();

  {
    IndexStore store(dir.path());
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE(store.Save(original));
  }

  // Reopen from scratch, so this really exercises the on-disk bytes.
  IndexStore store(dir.path());
  ASSERT_TRUE(store.ok());
  InvertedIndex loaded;
  ASSERT_TRUE(store.Load(&loaded));

  EXPECT_EQ(loaded.DocumentCount(), original.DocumentCount());
  EXPECT_EQ(loaded.UniqueTerms(), original.UniqueTerms());
  EXPECT_DOUBLE_EQ(loaded.AverageDocumentLength(), original.AverageDocumentLength());
  EXPECT_EQ(loaded.AllDocuments(), original.AllDocuments());
  EXPECT_EQ(loaded.DocumentFrequency("chunk"), original.DocumentFrequency("chunk"));
  EXPECT_EQ(loaded.DocumentsWithField("author", "ojas"),
            original.DocumentsWithField("author", "ojas"));
}

TEST(IndexStore, PreservesTombstonesAndTextForSnippets) {
  const TempDir dir("tombstones");
  {
    IndexStore store(dir.path());
    ASSERT_TRUE(store.Save(BuildIndex()));
  }
  IndexStore store(dir.path());
  InvertedIndex loaded;
  ASSERT_TRUE(store.Load(&loaded));

  EXPECT_EQ(loaded.DeletedDocumentCount(), 1U) << "the tombstone must survive a reload";
  EXPECT_FALSE(loaded.Text(0).empty()) << "document text is needed for snippets after reload";
}

TEST(IndexStore, PostingsSurviveWithPositions) {
  const TempDir dir("postings");
  {
    IndexStore store(dir.path());
    ASSERT_TRUE(store.Save(BuildIndex()));
  }
  IndexStore store(dir.path());
  InvertedIndex loaded;
  ASSERT_TRUE(store.Load(&loaded));

  const PostingList* postings = loaded.Lookup("chunk");
  ASSERT_NE(postings, nullptr);
  ASSERT_FALSE(postings->empty());
  EXPECT_FALSE(postings->front().positions.empty()) << "phrase search needs positions back";
}

// The point of persisting: a shard can answer queries after a restart without re-indexing.
TEST(IndexStore, ASearchEngineWorksAfterReload) {
  const TempDir dir("engine");
  {
    SearchEngine engine;
    engine.IndexDocument("wal.md", "A write ahead log makes writes crash consistent.");
    engine.IndexDocument("hashing.md", "Consistent hashing places chunks on a ring.");
    IndexStore store(dir.path());
    ASSERT_TRUE(store.Save(engine.index()));
  }

  IndexStore store(dir.path());
  InvertedIndex loaded;
  ASSERT_TRUE(store.Load(&loaded));

  const Ranker ranker(loaded);
  EXPECT_GT(ranker.InverseDocumentFrequency("hash"), 0.0);
  EXPECT_EQ(loaded.DocumentFrequency("hash"), 1U);

  // Phrase-critical data made it through: positions for a multi-term document.
  const PostingList* postings = loaded.Lookup("log");
  ASSERT_NE(postings, nullptr);
  EXPECT_EQ(postings->size(), 1U);
}

TEST(IndexStore, LoadFailsOnAnEmptyStoreAndLeavesTheTargetAlone) {
  const TempDir dir("empty");
  const IndexStore store(dir.path());
  ASSERT_TRUE(store.ok());

  InvertedIndex index;
  index.IndexDocument("keep.md", "chunk");
  EXPECT_FALSE(store.Load(&index)) << "nothing saved yet";
  EXPECT_EQ(index.DocumentCount(), 1U) << "a failed load must not clobber the index";
}

TEST(IndexStore, SavingTwiceOverwritesRatherThanAccumulates) {
  const TempDir dir("overwrite");
  IndexStore store(dir.path());

  InvertedIndex first;
  first.IndexDocument("a.md", "chunk");
  ASSERT_TRUE(store.Save(first));

  InvertedIndex second;
  second.IndexDocument("b.md", "ring");
  second.IndexDocument("c.md", "hashing");
  ASSERT_TRUE(store.Save(second));

  InvertedIndex loaded;
  ASSERT_TRUE(store.Load(&loaded));
  EXPECT_EQ(loaded.DocumentCount(), 2U);
}

// Saving twice into the same store must leave the second snapshot, not a union of both. Compact()
// drops posting lists whose documents are all gone; if a re-save doesn't remove those keys, Load()
// resurrects them — and after Compact() renumbered the doc ids, their postings point at whichever
// documents now occupy those slots, so a query returns the wrong files.
TEST(IndexStore, ResavingDropsTermsThatNoLongerExist) {
  const TempDir dir("resave");
  IndexStore store(dir.path());
  ASSERT_TRUE(store.ok());

  InvertedIndex index;
  index.IndexDocument("keep.md", "chunk replication");
  index.IndexDocument("gone.md", "zebra");  // "zebra" lives only in the doomed document
  ASSERT_TRUE(store.Save(index));

  index.DeleteDocument("gone.md");
  index.Compact();  // removes the now-empty "zebra" posting list
  ASSERT_EQ(index.Lookup("zebra"), nullptr);
  ASSERT_TRUE(store.Save(index));

  InvertedIndex loaded;
  ASSERT_TRUE(store.Load(&loaded));
  EXPECT_EQ(loaded.DocumentCount(), 1U);
  EXPECT_EQ(loaded.Lookup("zebra"), nullptr) << "a term dropped by Compact() must not come back";
}

// The damaging form of the same bug: when the compacted-away document sat in the middle, its doc
// ids get reused by other documents, so a resurrected posting list points at a *live* file that
// never contained the term.
TEST(IndexStore, ResurrectedPostingsDoNotPointAtTheWrongDocument) {
  const TempDir dir("resave_midlist");
  IndexStore store(dir.path());
  ASSERT_TRUE(store.ok());

  InvertedIndex index;
  index.IndexDocument("alpha.md", "alpha");
  index.IndexDocument("gone.md", "zebra");  // doc id 1
  index.IndexDocument("gamma.md", "gamma");
  index.IndexDocument("delta.md", "delta");
  ASSERT_TRUE(store.Save(index));

  index.DeleteDocument("gone.md");
  index.Compact();  // gamma.md slides into doc id 1
  ASSERT_TRUE(store.Save(index));

  InvertedIndex loaded;
  ASSERT_TRUE(store.Load(&loaded));
  ASSERT_EQ(loaded.DocumentCount(), 3U);
  const PostingList* zebra = loaded.Lookup("zebra");
  if (zebra != nullptr && !zebra->empty()) {
    const DocId doc_id = zebra->front().doc_id;
    ADD_FAILURE() << "\"zebra\" resurrected and now points at " << loaded.FileId(doc_id)
                  << ", which never contained it";
  }
}

}  // namespace atlas::search

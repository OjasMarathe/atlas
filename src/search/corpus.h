#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace atlas::search {

struct Document {
  std::string file_id;  // path relative to the corpus root
  std::string text;
};

// Reads a directory tree of plain-text documents (.md / .txt by default).
//
// Phase 3's stand-in for real ingestion: it lets the search shard be built and measured against
// a local corpus while the storage track lands in parallel. Phase 1's pipeline will call
// IndexDocument with chunk-derived text instead, and this loader becomes a test fixture.
std::vector<Document> LoadCorpus(std::string_view directory,
                                 const std::vector<std::string>& extensions = {".md", ".txt"});

}  // namespace atlas::search

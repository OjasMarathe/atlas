#include "search/corpus.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace atlas::search {

std::vector<Document> LoadCorpus(std::string_view directory,
                                 const std::vector<std::string>& extensions) {
  namespace fs = std::filesystem;
  std::vector<Document> documents;

  const fs::path root(directory);
  if (!fs::exists(root) || !fs::is_directory(root)) return documents;

  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const std::string extension = entry.path().extension().string();
    if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) continue;

    std::ifstream file(entry.path());
    if (!file) continue;
    std::ostringstream contents;
    contents << file.rdbuf();
    documents.push_back(
        Document{fs::relative(entry.path(), root).string(), std::move(contents).str()});
  }

  // Directory iteration order is unspecified; sort so DocIds (and therefore tie-breaks) are
  // reproducible across machines.
  std::sort(documents.begin(), documents.end(),
            [](const Document& lhs, const Document& rhs) { return lhs.file_id < rhs.file_id; });
  return documents;
}

}  // namespace atlas::search

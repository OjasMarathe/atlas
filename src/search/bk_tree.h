#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::search {

// Levenshtein edit distance: the fewest single-character insertions, deletions or substitutions
// that turn `a` into `b`.
std::size_t EditDistance(std::string_view a, std::string_view b);

struct Suggestion {
  std::string word;
  std::size_t distance;
};

// BK-tree — "which indexed words are within k edits of this typo?"
//
// Scanning the whole vocabulary answers that but costs an edit-distance computation per word.
// A BK-tree exploits the fact that edit distance is a *metric*, so the triangle inequality holds:
// if d(query, node) = d, any word within k edits of the query sits in a child branch whose edge
// label lies in [d-k, d+k]. Every other branch is provably empty and is never visited.
// See concepts/bk-tree-levenshtein.md.
class BkTree {
 public:
  void Insert(std::string_view word);

  // Words within `max_distance` edits, nearest first (ties broken alphabetically).
  std::vector<Suggestion> Search(std::string_view word, std::size_t max_distance) const;

  bool Empty() const { return root_ == nullptr; }
  std::size_t Size() const { return size_; }

 private:
  struct Node {
    std::string word;
    std::map<std::size_t, std::unique_ptr<Node>> children;  // edge label = distance to parent
  };

  std::unique_ptr<Node> root_;
  std::size_t size_ = 0;
};

}  // namespace atlas::search

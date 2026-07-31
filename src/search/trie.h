#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::search {

struct Completion {
  std::string word;
  std::uint64_t frequency;
};

// Prefix tree for autocomplete: "what words start with these letters?"
//
// A hash map can't answer that — hashing destroys the shared-prefix structure the question is
// about. A trie stores each word as a path of characters, so every completion of a prefix lives
// in one subtree, reachable after walking the prefix once.
// See concepts/trie-autocomplete.md.
class Trie {
 public:
  void Insert(std::string_view word, std::uint64_t frequency);

  // Up to `limit` completions of `prefix`, most frequent first (ties broken alphabetically).
  // The prefix itself is included when it is a word.
  std::vector<Completion> Complete(std::string_view prefix, std::size_t limit) const;

  std::size_t WordCount() const { return words_; }

 private:
  struct Node {
    std::map<char, std::unique_ptr<Node>> children;  // ordered, so traversal is alphabetical
    std::uint64_t frequency = 0;                     // > 0 marks the end of a word
  };

  static void Collect(const Node* node, std::string* buffer, std::vector<Completion>* out);

  Node root_;
  std::size_t words_ = 0;
};

}  // namespace atlas::search

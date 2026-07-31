#include "search/trie.h"

#include <algorithm>

namespace atlas::search {

void Trie::Insert(std::string_view word, std::uint64_t frequency) {
  if (word.empty() || frequency == 0) return;
  Node* node = &root_;
  for (const char ch : word) {
    std::unique_ptr<Node>& child = node->children[ch];
    if (!child) child = std::make_unique<Node>();
    node = child.get();
  }
  if (node->frequency == 0) ++words_;
  node->frequency = frequency;
}

void Trie::Collect(const Node* node, std::string* buffer, std::vector<Completion>* out) {
  if (node->frequency > 0) out->push_back(Completion{*buffer, node->frequency});
  for (const auto& [ch, child] : node->children) {
    buffer->push_back(ch);
    Collect(child.get(), buffer, out);
    buffer->pop_back();
  }
}

std::vector<Completion> Trie::Complete(std::string_view prefix, std::size_t limit) const {
  if (limit == 0) return {};

  // Walk the prefix once; everything below the node we land on shares it.
  const Node* node = &root_;
  for (const char ch : prefix) {
    const auto it = node->children.find(ch);
    if (it == node->children.end()) return {};
    node = it->second.get();
  }

  std::vector<Completion> found;
  std::string buffer(prefix);
  Collect(node, &buffer, &found);

  // Rank by frequency, then alphabetically so output is deterministic.
  const auto better = [](const Completion& lhs, const Completion& rhs) {
    if (lhs.frequency != rhs.frequency) return lhs.frequency > rhs.frequency;
    return lhs.word < rhs.word;
  };
  if (found.size() > limit) {
    std::partial_sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(limit),
                      found.end(), better);
    found.resize(limit);
  } else {
    std::sort(found.begin(), found.end(), better);
  }
  return found;
}

}  // namespace atlas::search

#include "search/bk_tree.h"

#include <algorithm>
#include <numeric>

namespace atlas::search {

std::size_t EditDistance(std::string_view a, std::string_view b) {
  if (a.empty()) return b.size();
  if (b.empty()) return a.size();

  // Two rows instead of the full matrix: each cell only depends on the row above and the cell
  // to its left, so O(min(n,m)) memory is enough.
  if (a.size() < b.size()) std::swap(a, b);
  std::vector<std::size_t> previous(b.size() + 1);
  std::vector<std::size_t> current(b.size() + 1);
  std::iota(previous.begin(), previous.end(), 0);

  for (std::size_t i = 1; i <= a.size(); ++i) {
    current[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      const std::size_t deletion = previous[j] + 1;
      const std::size_t insertion = current[j - 1] + 1;
      current[j] = std::min({substitution, deletion, insertion});
    }
    previous.swap(current);
  }
  return previous[b.size()];
}

void BkTree::Insert(std::string_view word) {
  if (word.empty()) return;
  if (root_ == nullptr) {
    root_ = std::make_unique<Node>();
    root_->word = std::string(word);
    size_ = 1;
    return;
  }

  Node* node = root_.get();
  while (true) {
    const std::size_t distance = EditDistance(word, node->word);
    if (distance == 0) return;  // already present
    std::unique_ptr<Node>& child = node->children[distance];
    if (!child) {
      child = std::make_unique<Node>();
      child->word = std::string(word);
      ++size_;
      return;
    }
    node = child.get();
  }
}

std::vector<Suggestion> BkTree::Search(std::string_view word, std::size_t max_distance) const {
  std::vector<Suggestion> found;
  if (root_ == nullptr) return found;

  std::vector<const Node*> stack{root_.get()};
  while (!stack.empty()) {
    const Node* node = stack.back();
    stack.pop_back();

    const std::size_t distance = EditDistance(word, node->word);
    if (distance <= max_distance) found.push_back(Suggestion{node->word, distance});

    // Triangle inequality: a word within max_distance of the query differs from this node by
    // between distance-max_distance and distance+max_distance. Branches outside that band cannot
    // contain a match, so they are skipped entirely — that pruning is the whole point.
    const std::size_t low = distance > max_distance ? distance - max_distance : 0;
    const std::size_t high = distance + max_distance;
    for (auto it = node->children.lower_bound(low); it != node->children.end() && it->first <= high;
         ++it) {
      stack.push_back(it->second.get());
    }
  }

  std::sort(found.begin(), found.end(), [](const Suggestion& lhs, const Suggestion& rhs) {
    if (lhs.distance != rhs.distance) return lhs.distance < rhs.distance;
    return lhs.word < rhs.word;
  });
  return found;
}

}  // namespace atlas::search

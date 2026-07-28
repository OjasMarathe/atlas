#include "search/postings.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace atlas::search {

SkipList::SkipList(const std::vector<DocId>& docs) {
  if (docs.empty()) return;
  stride_ = static_cast<std::size_t>(std::sqrt(static_cast<double>(docs.size())));
  if (stride_ < 1) stride_ = 1;
  for (std::size_t i = 0; i < docs.size(); i += stride_) checkpoints_.push_back(i);
}

std::size_t SkipList::Advance(const std::vector<DocId>& docs, std::size_t from,
                              DocId target) const {
  if (from >= docs.size()) return docs.size();

  std::size_t index = from;
  if (!checkpoints_.empty()) {
    // Leap forward while the *next* checkpoint still lands at or before the target: everything
    // it skips over is strictly less than target and can never match.
    std::size_t block = from / stride_;
    while (block + 1 < checkpoints_.size() && docs[checkpoints_[block + 1]] <= target) ++block;
    index = std::max(index, checkpoints_[block]);
  }
  while (index < docs.size() && docs[index] < target) ++index;
  return index;
}

std::vector<DocId> Intersect(const std::vector<DocId>& a, const std::vector<DocId>& b) {
  std::vector<DocId> out;
  if (a.empty() || b.empty()) return out;
  out.reserve(std::min(a.size(), b.size()));

  const SkipList skip_a(a);
  const SkipList skip_b(b);
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) {
      out.push_back(a[i]);
      ++i;
      ++j;
    } else if (a[i] < b[j]) {
      i = skip_a.Advance(a, i, b[j]);
    } else {
      j = skip_b.Advance(b, j, a[i]);
    }
  }
  return out;
}

std::vector<DocId> Union(const std::vector<DocId>& a, const std::vector<DocId>& b) {
  std::vector<DocId> out;
  out.reserve(a.size() + b.size());
  std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

std::vector<DocId> Difference(const std::vector<DocId>& a, const std::vector<DocId>& b) {
  std::vector<DocId> out;
  out.reserve(a.size());
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

}  // namespace atlas::search

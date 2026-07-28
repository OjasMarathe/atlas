#pragma once

#include <cstddef>
#include <vector>

#include "search/types.h"

namespace atlas::search {

// Skip pointers over a sorted doc-id list (Manning et al., IIR ch. 2).
//
// Intersecting two posting lists is a merge, and the merge stalls whenever one list has a long
// run of ids the other doesn't contain — a scan of postings that can never match. Skip pointers
// place a checkpoint every sqrt(n) postings so the merge can leap over such a run instead of
// walking it. sqrt(n) is the classic stride: it balances the number of checkpoints against the
// distance each one can save.
class SkipList {
 public:
  explicit SkipList(const std::vector<DocId>& docs);

  // Index of the first posting at or after `from` whose doc id is >= `target`
  // (docs.size() if there is none). `docs` must be the same list the SkipList was built over.
  std::size_t Advance(const std::vector<DocId>& docs, std::size_t from, DocId target) const;

  std::size_t stride() const { return stride_; }

 private:
  std::vector<std::size_t> checkpoints_;  // indices into the doc list, every stride_ postings
  std::size_t stride_ = 1;
};

// Boolean set operations over sorted, duplicate-free doc-id lists. Each returns a sorted list.
std::vector<DocId> Intersect(const std::vector<DocId>& a, const std::vector<DocId>& b);
std::vector<DocId> Union(const std::vector<DocId>& a, const std::vector<DocId>& b);
std::vector<DocId> Difference(const std::vector<DocId>& a, const std::vector<DocId>& b);

}  // namespace atlas::search

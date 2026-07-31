#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::query {

// Parsed query tree.
//
// Deliberately independent of the search shard: Phase 4's coordinator parses a query *once* and
// ships this structure to every shard, instead of each shard re-parsing the same string. Until
// the coordinator exists the shard calls the parser itself — same code, one implementation.
struct Node {
  enum class Kind { Term, And, Or, Not, Phrase, Field };

  Kind kind = Kind::Term;
  std::string term;   // Term: the analyzed (stemmed) term. Field: the value, matched verbatim.
  std::string field;  // Field only: which attribute to match (author/type/lang/...).

  // Phrase children only: the term's offset within the quoted phrase, counting words that
  // analyzed away. Document positions are recorded pre-filter for exactly this reason, so
  // "chunk ring" cannot match "chunk of the ring".
  std::uint32_t phrase_offset = 0;

  std::vector<std::unique_ptr<Node>> children;  // And/Or/Phrase: >= 1. Not: exactly 1.

  static std::unique_ptr<Node> MakeTerm(std::string text);
  static std::unique_ptr<Node> MakeField(std::string field, std::string value);
  static std::unique_ptr<Node> Make(Kind kind, std::vector<std::unique_ptr<Node>> children);
};

struct Options {
  // Operator applied between adjacent terms with no explicit keyword ("chunk replication").
  // Defaults to Or: BM25 ranks the results, so widening recall and letting the ranker sort it
  // out beats returning nothing because one term was missing.
  Node::Kind implicit_operator = Node::Kind::Or;

  // Maps a raw query word to the term(s) actually stored in the index. Return empty to drop the
  // word (a stop word); return several when one word analyzes into several index terms
  // ("write-ahead" -> {write, ahead}), which the parser combines with AND.
  //
  // Injected rather than called directly so this module stays free of any search-shard
  // dependency — the caller passes the *same* analyzer used at index time, which is what keeps
  // the query and document paths symmetric. Identity when unset.
  std::function<std::vector<std::string>(std::string_view)> analyze_term;
};

struct Result {
  std::unique_ptr<Node> root;  // null if the query has no searchable terms, or on error
  std::string error;           // empty on success
  bool ok() const { return error.empty(); }
};

// Grammar (recursive descent):
//   expr   := and_expr ( "OR" and_expr )*
//   and    := unary ( ( "AND" | <implicit> ) unary )*
//   unary  := "NOT" unary | "(" expr ")" | '"' PHRASE '"' | FIELD ":" VALUE | TERM
//
// Operators are recognized as UPPERCASE keywords *before* text analysis runs — lowercase "not"
// is an ordinary word (and a stop word). See docs/concepts/tokenization-stemming.md.
// Terms are analyzed with the same pipeline used at index time; terms that analyze away (stop
// words) are dropped, and a clause left empty by that is dropped with it.
Result Parse(std::string_view query, Options options = {});

// Every Term leaf under `root` that should contribute to the relevance score: negated subtrees
// are skipped (scoring a document on a term it was selected for *not* having is incoherent) and
// so are Field filters (they select, they don't rank). Phrase terms count. Duplicates removed,
// order preserved.
std::vector<std::string> PositiveTerms(const Node* root);

// True when the query positively *selects* documents by something other than relevance — a
// Field filter outside a NOT. It distinguishes two queries that both have nothing to rank on:
// "author:ojas" is a meaningful request for a document set, whereas a bare "NOT x" is an
// unranked dump of almost the whole shard and is nearly always a mistake.
bool HasPositiveFilter(const Node* root);

}  // namespace atlas::query

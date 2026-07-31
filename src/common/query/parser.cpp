#include "common/query/parser.h"

#include <cctype>
#include <unordered_set>
#include <utility>

namespace atlas::query {
namespace {

enum class TokenKind { Term, And, Or, Not, LParen, RParen, Phrase, Field };

struct Token {
  TokenKind kind;
  std::string text;   // Term/Phrase: the words. Field: the value.
  std::string field;  // Field only
};

// Operators are UPPERCASE keywords, matched here — before any analysis — so that a lowercase
// "not" stays an ordinary (stop) word rather than silently becoming an operator.
TokenKind ClassifyWord(const std::string& word) {
  if (word == "AND") return TokenKind::And;
  if (word == "OR") return TokenKind::Or;
  if (word == "NOT") return TokenKind::Not;
  return TokenKind::Term;
}

std::vector<Token> Lex(std::string_view query) {
  std::vector<Token> tokens;
  std::string current;

  const auto flush = [&] {
    if (current.empty()) return;
    const TokenKind kind = ClassifyWord(current);
    if (kind != TokenKind::Term) {
      tokens.push_back(Token{kind, {}, {}});
      current.clear();
      return;
    }
    // "author:ojas" is a field filter; a bare colon or an empty side is just an odd word.
    const std::size_t colon = current.find(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < current.size()) {
      tokens.push_back(
          Token{TokenKind::Field, current.substr(colon + 1), current.substr(0, colon)});
    } else {
      tokens.push_back(Token{TokenKind::Term, current, {}});
    }
    current.clear();
  };

  for (std::size_t i = 0; i < query.size(); ++i) {
    const char ch = query[i];
    if (ch == '"') {
      flush();
      // Everything up to the closing quote is one phrase token; an unterminated quote simply
      // runs to the end of the query rather than failing.
      const std::size_t close = query.find('"', i + 1);
      const std::size_t end = close == std::string_view::npos ? query.size() : close;
      tokens.push_back(Token{TokenKind::Phrase, std::string(query.substr(i + 1, end - i - 1)), {}});
      i = end;
    } else if (ch == '(' || ch == ')') {
      flush();
      tokens.push_back(Token{ch == '(' ? TokenKind::LParen : TokenKind::RParen, {}, {}});
    } else if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      flush();
    } else {
      current.push_back(ch);
    }
  }
  flush();
  return tokens;
}

// Recursive descent over the token stream. A null return means "this clause analyzed away"
// (every term in it was a stop word), which is different from a parse error — errors are
// reported through error_.
class Parser {
 public:
  Parser(const std::vector<Token>& tokens, const Options& options)
      : tokens_(tokens), options_(options) {}

  std::unique_ptr<Node> ParseExpression() {
    std::vector<std::unique_ptr<Node>> children;
    Collect(children, ParseConjunction());
    while (error_.empty()) {
      if (Peek(TokenKind::Or)) {
        ++pos_;
        Collect(children, ParseConjunction());
      } else if (options_.implicit_operator == Node::Kind::Or && StartsUnary()) {
        Collect(children, ParseConjunction());
      } else {
        break;
      }
    }
    return Combine(Node::Kind::Or, std::move(children));
  }

  const std::string& error() const { return error_; }
  bool AtEnd() const { return pos_ >= tokens_.size(); }

 private:
  const std::vector<Token>& tokens_;
  const Options& options_;
  std::size_t pos_ = 0;
  std::string error_;

  bool Peek(TokenKind kind) const { return pos_ < tokens_.size() && tokens_[pos_].kind == kind; }

  bool StartsUnary() const {
    return Peek(TokenKind::Term) || Peek(TokenKind::Not) || Peek(TokenKind::LParen);
  }

  static void Collect(std::vector<std::unique_ptr<Node>>& into, std::unique_ptr<Node> node) {
    if (node != nullptr) into.push_back(std::move(node));
  }

  // One surviving child needs no operator wrapper; none means the whole clause dropped out.
  static std::unique_ptr<Node> Combine(Node::Kind kind,
                                       std::vector<std::unique_ptr<Node>> children) {
    if (children.empty()) return nullptr;
    if (children.size() == 1) return std::move(children.front());
    return Node::Make(kind, std::move(children));
  }

  std::unique_ptr<Node> ParseConjunction() {
    std::vector<std::unique_ptr<Node>> children;
    Collect(children, ParseUnary());
    while (error_.empty()) {
      if (Peek(TokenKind::And)) {
        ++pos_;
        Collect(children, ParseUnary());
      } else if (options_.implicit_operator == Node::Kind::And && StartsUnary()) {
        Collect(children, ParseUnary());
      } else {
        break;
      }
    }
    return Combine(Node::Kind::And, std::move(children));
  }

  // A quoted phrase becomes a Phrase node whose children carry their offset *within the phrase*,
  // counting words that analyze away. Document positions are recorded pre-filter, so keeping the
  // gaps here is what stops "chunk ring" from matching "chunk of the ring".
  std::unique_ptr<Node> ParsePhrase(const std::string& text) {
    std::vector<std::unique_ptr<Node>> parts;
    std::uint32_t offset = 0;
    std::string word;
    const auto take = [&] {
      if (word.empty()) return;
      std::vector<std::string> analyzed;
      if (options_.analyze_term) {
        analyzed = options_.analyze_term(word);
      } else {
        analyzed.push_back(word);
      }
      for (std::string& term : analyzed) {
        auto node = Node::MakeTerm(std::move(term));
        node->phrase_offset = offset++;
        parts.push_back(std::move(node));
      }
      // A word that analyzed away (a stop word) still consumed a slot in the document's token
      // stream, so advance past it.
      if (analyzed.empty()) ++offset;
      word.clear();
    };
    for (const char ch : text) {
      if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
        take();
      } else {
        word.push_back(ch);
      }
    }
    take();

    if (parts.empty()) return nullptr;  // phrase was entirely stop words
    if (parts.size() == 1) {
      parts.front()->phrase_offset = 0;
      return std::move(parts.front());  // a one-word phrase is just a term
    }
    return Node::Make(Node::Kind::Phrase, std::move(parts));
  }

  std::unique_ptr<Node> ParseUnary() {
    if (!error_.empty()) return nullptr;
    if (pos_ >= tokens_.size()) {
      error_ = "unexpected end of query";
      return nullptr;
    }

    const Token& token = tokens_[pos_];
    switch (token.kind) {
      case TokenKind::Not: {
        ++pos_;
        std::unique_ptr<Node> child = ParseUnary();
        if (!error_.empty()) return nullptr;
        // NOT over a clause that analyzed away excludes nothing, so drop it entirely.
        if (child == nullptr) return nullptr;
        std::vector<std::unique_ptr<Node>> children;
        children.push_back(std::move(child));
        return Node::Make(Node::Kind::Not, std::move(children));
      }
      case TokenKind::LParen: {
        ++pos_;
        // An empty group is punctuation, not a query: someone searching "fsync()" wants fsync,
        // not a syntax error. Drop it the same way a stop word is dropped.
        if (Peek(TokenKind::RParen)) {
          ++pos_;
          return nullptr;
        }
        std::unique_ptr<Node> inner = ParseExpression();
        if (!error_.empty()) return nullptr;
        if (!Peek(TokenKind::RParen)) {
          error_ = "missing closing parenthesis";
          return nullptr;
        }
        ++pos_;
        return inner;
      }
      case TokenKind::Term: {
        ++pos_;
        std::vector<std::string> analyzed;
        if (options_.analyze_term) {
          analyzed = options_.analyze_term(token.text);
        } else {
          analyzed.emplace_back(token.text);
        }
        if (analyzed.empty()) return nullptr;  // stop word
        if (analyzed.size() == 1) return Node::MakeTerm(std::move(analyzed.front()));

        // One word that analyzes into several index terms ("write-ahead" -> write, ahead). The
        // document indexed them adjacently, so the precise query is a phrase — AND is the
        // closest 3a approximation until positional phrase matching lands in 3b.
        std::vector<std::unique_ptr<Node>> parts;
        parts.reserve(analyzed.size());
        for (std::string& term : analyzed) parts.push_back(Node::MakeTerm(std::move(term)));
        return Node::Make(Node::Kind::And, std::move(parts));
      }
      case TokenKind::Phrase: {
        ++pos_;
        return ParsePhrase(token.text);
      }
      case TokenKind::Field: {
        ++pos_;
        return Node::MakeField(token.field, token.text);
      }
      case TokenKind::RParen:
        error_ = "unexpected ')'";
        return nullptr;
      case TokenKind::And:
      case TokenKind::Or:
        error_ = "missing operand before operator";
        return nullptr;
    }
    error_ = "unrecognized token";
    return nullptr;
  }
};

void CollectPositiveTerms(const Node* node, std::vector<std::string>& out,
                          std::unordered_set<std::string>& seen) {
  if (node == nullptr) return;
  switch (node->kind) {
    case Node::Kind::Term:
      if (seen.insert(node->term).second) out.push_back(node->term);
      return;
    case Node::Kind::Not:
      return;  // excluded terms must not contribute to the relevance score
    case Node::Kind::Field:
      return;  // filters select documents, they don't rank them
    case Node::Kind::And:
    case Node::Kind::Or:
    case Node::Kind::Phrase:
      for (const auto& child : node->children) CollectPositiveTerms(child.get(), out, seen);
      return;
  }
}

}  // namespace

std::unique_ptr<Node> Node::MakeTerm(std::string text) {
  auto node = std::make_unique<Node>();
  node->kind = Kind::Term;
  node->term = std::move(text);
  return node;
}

std::unique_ptr<Node> Node::MakeField(std::string field, std::string value) {
  auto node = std::make_unique<Node>();
  node->kind = Kind::Field;
  node->field = std::move(field);
  node->term = std::move(value);
  return node;
}

std::unique_ptr<Node> Node::Make(Kind kind, std::vector<std::unique_ptr<Node>> children) {
  auto node = std::make_unique<Node>();
  node->kind = kind;
  node->children = std::move(children);
  return node;
}

Result Parse(std::string_view query, Options options) {
  const std::vector<Token> tokens = Lex(query);
  if (tokens.empty()) return Result{nullptr, {}};

  Parser parser(tokens, options);
  std::unique_ptr<Node> root = parser.ParseExpression();
  if (!parser.error().empty()) return Result{nullptr, parser.error()};
  if (!parser.AtEnd()) return Result{nullptr, "unexpected ')'"};
  return Result{std::move(root), {}};
}

std::vector<std::string> PositiveTerms(const Node* root) {
  std::vector<std::string> terms;
  std::unordered_set<std::string> seen;
  CollectPositiveTerms(root, terms, seen);
  return terms;
}

}  // namespace atlas::query

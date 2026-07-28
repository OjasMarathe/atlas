#include "common/query/parser.h"

#include <cctype>
#include <unordered_set>
#include <utility>

namespace atlas::query {
namespace {

enum class TokenKind { Term, And, Or, Not, LParen, RParen };

struct Token {
  TokenKind kind;
  std::string text;  // Term only
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
    tokens.push_back(Token{kind, kind == TokenKind::Term ? current : std::string{}});
    current.clear();
  };
  for (const char ch : query) {
    if (ch == '(' || ch == ')') {
      flush();
      tokens.push_back(Token{ch == '(' ? TokenKind::LParen : TokenKind::RParen, {}});
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
        std::string analyzed =
            options_.analyze_term ? options_.analyze_term(token.text) : token.text;
        if (analyzed.empty()) return nullptr;  // stop word
        return Node::MakeTerm(std::move(analyzed));
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
    case Node::Kind::And:
    case Node::Kind::Or:
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

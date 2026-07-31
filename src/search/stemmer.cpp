#include "search/stemmer.h"

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace atlas::search {
namespace {

using Rule = std::pair<std::string_view, std::string_view>;

// Porter's algorithm treats the word as a mutable buffer and walks five ordered phases, each
// testing suffixes against a "measure" of the stem that would remain. Rule tables and phase
// order follow Porter (1980); see docs/concepts/tokenization-stemming.md for the walkthrough.
class Porter {
 public:
  explicit Porter(std::string word) : b_(std::move(word)) {}

  std::string Run() {
    if (b_.size() <= 2) return b_;
    Step1a();
    Step1b();
    Step1c();
    Step2();
    Step3();
    Step4();
    Step5a();
    Step5b();
    return b_;
  }

 private:
  std::string b_;

  // A consonant is any letter other than a/e/i/o/u, and other than 'y' preceded by a
  // consonant — so 'y' is a vowel in "try" but a consonant in "toy" and "yes".
  bool Cons(std::size_t i) const {
    switch (b_[i]) {
      case 'a':
      case 'e':
      case 'i':
      case 'o':
      case 'u':
        return false;
      case 'y':
        return i == 0 || !Cons(i - 1);
      default:
        return true;
    }
  }

  // Porter's measure m: the number of vowel-consonant sequences in b_[0, n), i.e. the count of
  // VC groups in the form [C](VC)^m[V]. Roughly "how many syllables of stem are left".
  int Measure(std::size_t n) const {
    int m = 0;
    std::size_t i = 0;
    while (i < n && Cons(i)) ++i;
    while (i < n) {
      while (i < n && !Cons(i)) ++i;
      if (i >= n) break;
      while (i < n && Cons(i)) ++i;
      ++m;
    }
    return m;
  }

  bool HasVowel(std::size_t n) const {
    for (std::size_t i = 0; i < n; ++i) {
      if (!Cons(i)) return true;
    }
    return false;
  }

  bool EndsDoubleConsonant(std::size_t n) const {
    return n >= 2 && b_[n - 1] == b_[n - 2] && Cons(n - 1);
  }

  // Porter's *o test: the stem ends consonant-vowel-consonant where the final consonant is not
  // w, x or y. Marks stems that need a restored 'e' ("hop" -> "hope").
  bool EndsCvc(std::size_t n) const {
    if (n < 3) return false;
    if (!Cons(n - 3) || Cons(n - 2) || !Cons(n - 1)) return false;
    const char last = b_[n - 1];
    return last != 'w' && last != 'x' && last != 'y';
  }

  bool Ends(std::string_view suffix) const {
    return b_.size() >= suffix.size() &&
           b_.compare(b_.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  void Replace(std::string_view suffix, std::string_view replacement) {
    b_.resize(b_.size() - suffix.size());
    b_ += replacement;
  }

  // Applies the first matching rule whose remaining stem has measure > min_measure.
  void ApplyFirst(std::span<const Rule> rules, int min_measure) {
    for (const auto& [suffix, replacement] : rules) {
      if (!Ends(suffix)) continue;
      if (Measure(b_.size() - suffix.size()) > min_measure) Replace(suffix, replacement);
      return;
    }
  }

  // Plurals.
  void Step1a() {
    if (Ends("sses")) {
      Replace("sses", "ss");
    } else if (Ends("ies")) {
      Replace("ies", "i");
    } else if (Ends("ss")) {
      // "caress" keeps its double s.
    } else if (Ends("s")) {
      Replace("s", "");
    }
  }

  // Past participles and progressives, plus the cleanup that repairs the stem afterwards.
  void Step1b() {
    bool stripped = false;
    if (Ends("eed")) {
      if (Measure(b_.size() - 3) > 0) Replace("eed", "ee");
    } else if (Ends("ed")) {
      if (HasVowel(b_.size() - 2)) {
        Replace("ed", "");
        stripped = true;
      }
    } else if (Ends("ing")) {
      if (HasVowel(b_.size() - 3)) {
        Replace("ing", "");
        stripped = true;
      }
    }
    if (!stripped) return;

    if (Ends("at")) {
      Replace("at", "ate");
    } else if (Ends("bl")) {
      Replace("bl", "ble");
    } else if (Ends("iz")) {
      Replace("iz", "ize");
    } else if (EndsDoubleConsonant(b_.size())) {
      const char last = b_.back();
      if (last != 'l' && last != 's' && last != 'z') b_.pop_back();
    } else if (Measure(b_.size()) == 1 && EndsCvc(b_.size())) {
      b_ += 'e';
    }
  }

  // Terminal y -> i, so "happy" and "happiness" share a stem.
  void Step1c() {
    if (Ends("y") && HasVowel(b_.size() - 1)) Replace("y", "i");
  }

  // Derivational suffixes, longest first.
  void Step2() {
    static constexpr std::array<Rule, 21> kRules{{
        {"ational", "ate"}, {"tional", "tion"}, {"enci", "ence"},   {"anci", "ance"},
        {"izer", "ize"},    {"abli", "able"},   {"alli", "al"},     {"entli", "ent"},
        {"eli", "e"},       {"ousli", "ous"},   {"ization", "ize"}, {"ation", "ate"},
        {"ator", "ate"},    {"alism", "al"},    {"iveness", "ive"}, {"fulness", "ful"},
        {"ousness", "ous"}, {"aliti", "al"},    {"iviti", "ive"},   {"biliti", "ble"},
        {"logi", "log"},
    }};
    ApplyFirst(kRules, 0);
  }

  void Step3() {
    static constexpr std::array<Rule, 7> kRules{{
        {"icate", "ic"},
        {"ative", ""},
        {"alize", "al"},
        {"iciti", "ic"},
        {"ical", "ic"},
        {"ful", ""},
        {"ness", ""},
    }};
    ApplyFirst(kRules, 0);
  }

  // Strips residual suffixes, but only from stems with enough substance left (m > 1).
  void Step4() {
    if (Ends("ion")) {
      const std::size_t stem = b_.size() - 3;
      if (Measure(stem) > 1 && stem > 0 && (b_[stem - 1] == 's' || b_[stem - 1] == 't')) {
        Replace("ion", "");
        return;
      }
    }
    static constexpr std::array<Rule, 18> kRules{{
        {"al", ""},
        {"ance", ""},
        {"ence", ""},
        {"er", ""},
        {"ic", ""},
        {"able", ""},
        {"ible", ""},
        {"ant", ""},
        {"ement", ""},
        {"ment", ""},
        {"ent", ""},
        {"ou", ""},
        {"ism", ""},
        {"ate", ""},
        {"iti", ""},
        {"ous", ""},
        {"ive", ""},
        {"ize", ""},
    }};
    ApplyFirst(kRules, 1);
  }

  // Trailing e survives only where dropping it would leave an unpronounceable stem.
  void Step5a() {
    if (!Ends("e")) return;
    const std::size_t stem = b_.size() - 1;
    const int m = Measure(stem);
    if (m > 1 || (m == 1 && !EndsCvc(stem))) b_.pop_back();
  }

  // "controll" -> "control", but "roll" stays (m is too small).
  void Step5b() {
    if (Measure(b_.size()) > 1 && EndsDoubleConsonant(b_.size()) && b_.back() == 'l') {
      b_.pop_back();
    }
  }
};

}  // namespace

std::string Stem(std::string_view word) { return Porter(std::string(word)).Run(); }

}  // namespace atlas::search

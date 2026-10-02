// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>

#include "core/inference/unicode_tables.h"

// Letter, whitespace and lowercase predicates over the whole of Unicode, taken
// from Python's str methods by tools/gen_unicode_tables.py. Only the rules that
// are not per-code-point live here: U+0130, final sigma and the Turkic I.
//
// Unicode's complete set of context- or language-dependent lowercase rules
// (SpecialCasing.txt) is final sigma, Turkish/Azerbaijani and Lithuanian.
// Lithuanian is not implemented. Greek and every other language lowercase per
// code point, which the generated table covers.
//
// What this cannot do is split words in scripts written without spaces (Thai,
// Chinese, Japanese). That needs dictionary segmentation - ICU's BreakIterator
// plus its data bundle - and is the point at which ICU is worth adopting. The
// ICU build this file replaced is in commit 758ebf0.
namespace thespeon::unicode {
namespace detail {

template <std::size_t N>
constexpr bool InRanges(const CodePointRange (&table)[N], char32_t value) {
  const auto* after = std::upper_bound(
      std::begin(table), std::end(table), value,
      [](char32_t v, const CodePointRange& range) { return v < range.first; });
  return after != std::begin(table) && value <= std::prev(after)->last;
}

}  // namespace detail

// Python's str.isalpha(): Unicode general category L.
constexpr bool IsLetter(char32_t value) {
  return detail::InRanges(kLetters, value);
}

// Python's str.isspace().
constexpr bool IsSpace(char32_t value) {
  return detail::InRanges(kSpaces, value);
}

// A combining mark: general category M (Mn, Mc, Me).
constexpr bool IsMark(char32_t value) {
  return detail::InRanges(kMarks, value);
}

// The single-code-point lowercase, or the value itself when it has none. U+0130
// is left unchanged here because its lowercase is two code points; ToLower
// handles it.
constexpr char32_t ToLowerSimple(char32_t value) {
  const auto* after = std::upper_bound(
      std::begin(kLowercase), std::end(kLowercase), value,
      [](char32_t v, const LowercaseRun& run) { return v < run.first; });
  if (after == std::begin(kLowercase)) return value;
  const auto& run = *std::prev(after);
  if (value > run.last || (value - run.first) % run.stride != 0) return value;
  return static_cast<char32_t>(static_cast<std::int32_t>(value) + run.delta);
}

// Python's str.lower(). With `turkic`, also the Turkish and Azerbaijani rules
// for dotted and dotless I, which Python never applies: I -> U+0131, U+0130 -> i,
// and I followed by U+0307 COMBINING DOT ABOVE -> i.
//
// ICU additionally drops a U+0307 separated from the I by other non-above marks.
// That sequence is vanishingly rare in real text and is not handled.
inline std::u32string ToLower(const std::u32string& text,
                              bool turkic = false) {
  constexpr char32_t kCapitalSigma = 0x03a3;
  constexpr char32_t kDotAbove = 0x0307;

  // Unicode's Final_Sigma condition, exactly as Python evaluates it.
  const auto is_final_sigma = [&](std::size_t index) {
    std::size_t before = index;
    while (before > 0 && detail::InRanges(kCaseIgnorable, text[before - 1]))
      --before;
    if (before == 0 || !detail::InRanges(kCased, text[before - 1]))
      return false;
    std::size_t after = index + 1;
    while (after < text.size() && detail::InRanges(kCaseIgnorable, text[after]))
      ++after;
    return after == text.size() || !detail::InRanges(kCased, text[after]);
  };

  std::u32string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto value = text[index];
    if (value == 0x0130) {  // CAPITAL I WITH DOT ABOVE
      result.push_back(U'i');
      if (!turkic) result.push_back(kDotAbove);
    } else if (turkic && value == U'I') {
      if (index + 1 < text.size() && text[index + 1] == kDotAbove) {
        result.push_back(U'i');
        ++index;
      } else {
        result.push_back(0x0131);  // SMALL DOTLESS I
      }
    } else if (value == kCapitalSigma) {
      result.push_back(is_final_sigma(index) ? 0x03c2 : 0x03c3);
    } else {
      result.push_back(ToLowerSimple(value));
    }
  }
  return result;
}

}  // namespace thespeon::unicode

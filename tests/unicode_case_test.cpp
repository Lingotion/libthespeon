// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/unicode_case.h"

#include <cstdint>
#include <iterator>

#include <gtest/gtest.h>

namespace thespeon::unicode {
namespace {

// Mirrors fingerprint() in tools/gen_unicode_tables.py, which computes the same
// hash from Python's str methods. Matching it means IsLetter, IsSpace, IsMark
// and ToLower agree with Python on every code point, including whether each one
// makes a neighbouring capital sigma final.
std::uint64_t PythonFingerprint() {
  std::uint64_t value = 0xcbf29ce484222325ull;
  const auto mix = [&](std::uint32_t word) {
    for (int shift = 0; shift < 32; shift += 8) {
      value ^= (word >> shift) & 0xff;
      value *= 0x100000001b3ull;
    }
  };
  constexpr char32_t kCapitalSigma = 0x03a3;
  constexpr char32_t kFinalSigma = 0x03c2;
  for (char32_t code_point = 0; code_point < 0x110000; ++code_point) {
    mix(static_cast<std::uint32_t>(IsLetter(code_point)) |
        static_cast<std::uint32_t>(IsSpace(code_point)) << 1 |
        static_cast<std::uint32_t>(IsMark(code_point)) << 2);
    const auto lowered = ToLower(std::u32string(1, code_point));
    mix(static_cast<std::uint32_t>(lowered.size()));
    for (const auto character : lowered) mix(character);
    const std::u32string letter_before = {U'A', code_point, kCapitalSigma};
    const std::u32string alone_before = {code_point, kCapitalSigma};
    const std::u32string after = {U'A', kCapitalSigma, code_point};
    mix(static_cast<std::uint32_t>(ToLower(letter_before).back() == kFinalSigma) |
        static_cast<std::uint32_t>(ToLower(alone_before).back() == kFinalSigma)
            << 1 |
        static_cast<std::uint32_t>(ToLower(after)[1] == kFinalSigma) << 2);
  }
  return value;
}

TEST(UnicodeTables, MatchPythonOnEveryCodePoint) {
  EXPECT_EQ(PythonFingerprint(), kPythonFingerprint)
      << "unicode_case.h disagrees with the Python that generated "
         "unicode_tables.h (Unicode "
      << kUnicodeVersion << ")";
}

TEST(UnicodeTables, AreSortedAndDisjoint) {
  const auto check = [](const auto& table) {
    for (auto it = std::begin(table); it != std::end(table); ++it) {
      EXPECT_LE(it->first, it->last);
      if (it != std::begin(table)) EXPECT_LT(std::prev(it)->last, it->first);
    }
  };
  check(kLetters);
  check(kSpaces);
  check(kMarks);
  check(kLowercase);
  check(kCaseIgnorable);
  check(kCased);
}

TEST(ToLower, AppliesFinalSigmaAcrossIgnorables) {
  // The apostrophe and the combining acute are case-ignorable, so they are
  // looked through in both directions.
  EXPECT_EQ(ToLower(U"ΟΔΟΣ'"), U"οδος'");
  EXPECT_EQ(ToLower(U"Σ'Α"), U"σ'α");
  EXPECT_EQ(ToLower(U"ΑΣ́Β"), U"ασ́β");
  EXPECT_EQ(ToLower(U"ΑΣ́"), U"ας́");
  EXPECT_EQ(ToLower(U"Σ"), U"σ");
}

// Expected values are ICU's with the "tr" locale.
TEST(ToLower, AppliesTurkicDottedAndDotlessI) {
  EXPECT_EQ(ToLower(U"KIŞ İSTANBUL", true), U"kış istanbul");
  EXPECT_EQ(ToLower(U"İ", true), U"i");
  EXPECT_EQ(ToLower(U"KIŞ İSTANBUL"), U"kiş i̇stanbul");
}

}  // namespace
}  // namespace thespeon::unicode

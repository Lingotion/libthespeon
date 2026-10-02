// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/text_preprocess.h"

#include <stdexcept>

#include <gtest/gtest.h>

namespace thespeon::inference_detail {
namespace {

NormalizedText Normalize(const std::string& text, bool custom = false) {
  return NormalizeSegment(text, false, false, custom, 0);
}

TEST(Utf8, RoundTripsEveryEncodedLength) {
  const std::u32string text = U"aé⏸\U0001f600";
  std::string encoded;
  for (const auto code_point : text) encoded += EncodeUtf8(code_point);
  EXPECT_EQ(encoded, "a\xc3\xa9\xe2\x8f\xb8\xf0\x9f\x98\x80");
  EXPECT_EQ(DecodeUtf8(encoded), text);
}

TEST(Utf8, ReplacesInvalidBytes) {
  EXPECT_EQ(DecodeUtf8("a\xff" "b"), U"a�b");
  EXPECT_EQ(DecodeUtf8("\xe2\x82"), U"��");
  EXPECT_EQ(DecodeUtf8("\xc3" "a"), U"�a");
}

TEST(NormalizeSegment, CollapsesWhitespaceAndLowercases) {
  EXPECT_EQ(Normalize("  Hello \t\n  WORLD  ").text, "hello world");
  EXPECT_EQ(Normalize("ÄØ").text, "äø");
}

TEST(NormalizeSegment, KeepsOneBoundarySpaceWhenAsked) {
  EXPECT_EQ(NormalizeSegment("  a  ", true, true, false, 0).text, " a ");
  EXPECT_EQ(NormalizeSegment("  a  ", true, false, false, 0).text, " a");
  EXPECT_EQ(NormalizeSegment("  a  ", false, true, false, 0).text, "a ");
}

TEST(NormalizeSegment, FoldsTypographicApostrophes) {
  EXPECT_EQ(Normalize("Don’t ‘xʼ").text, "don't 'x'");
}

// The set all three reference grapheme preprocessors fold. Custom-pronounced
// text bypasses this fold, while U+0027 is already the grapheme target.
const std::u32string kAmbiguousApostrophes =
    U"\u2018\u2019\u201b\u02bc\u02bb\uff07\u0060\u00b4\u2032\u275b\u275c"
    U"\u02c8\u02ca\u02cb\u1fef\u1ffd\u1fbf\u1ffe\u0374\u0384\u055a"
    U"\u07f4\u07f5\u05f3\u05f4\ufe32";

std::string Utf8(const std::u32string& text) {
  std::string result;
  for (const auto code_point : text) result += EncodeUtf8(code_point);
  return result;
}

TEST(NormalizeSegment, FoldsEveryAmbiguousApostrophe) {
  ASSERT_EQ(kAmbiguousApostrophes.size(), 26u);
  EXPECT_EQ(Normalize(Utf8(kAmbiguousApostrophes)).text, std::string(26, '\''));
}

// Custom-pronounced segments carry a phonetic spelling verbatim, so grapheme
// cleanup is skipped for them, matching the Python service.
TEST(NormalizeSegment, LeavesCustomPronouncedGraphemesAlone) {
  EXPECT_EQ(Normalize("hɛLO’", true).text, "hɛLO’");
  EXPECT_EQ(Normalize(Utf8(kAmbiguousApostrophes), true).text,
            Utf8(kAmbiguousApostrophes));
}

// Secondary stress is not in the fold set and IPA Extensions have no case
// mappings, so a real pronunciation survives both paths untouched.
TEST(NormalizeSegment, LeavesIpaExtensionsAlone) {
  EXPECT_EQ(Normalize("ɑɐʃɒɜˌ", true).text,
            "ɑɐʃɒɜˌ");
  EXPECT_EQ(Normalize("ɑɐʃɒɜˌ").text,
            "ɑɐʃɒɜˌ");
}

TEST(NormalizeSegment, LowercasesLatinExtended) {
  // Ÿ maps backward to U+00FF and Ǆ to the digraph U+01C6; İ expands to two code
  // points, which is why LowercasesToLongerText exists.
  EXPECT_EQ(Normalize("ĀĹŊŹİŸǄ").text,
            "āĺŋźi\u0307ÿǆ");
  // Already lowercase, or with no uppercase partner at all.
  EXPECT_EQ(Normalize("ıĸŉſ").text,
            "ıĸŉſ");
}

// Case mapping is context-sensitive, which per-code-point mapping cannot do: a
// word-final sigma takes the final form, and a non-final one does not.
TEST(NormalizeSegment, LowercasesGreekSigmaByPosition) {
  EXPECT_EQ(Normalize("ΘΕΟΣ").text, "θεος");
  EXPECT_EQ(Normalize("ΣΟΦΙΑ").text, "σοφια");
}

// Case mapping can also lengthen the text: U+0130 becomes i + U+0307.
TEST(NormalizeSegment, LowercasesToLongerText) {
  EXPECT_EQ(Normalize("İSTANBUL").text, "i\u0307stanbul");
}

TEST(NormalizeSegment, CollapsesUnicodeWhitespace) {
  EXPECT_EQ(Normalize("a b c　d e").text, "a b c d e");
  EXPECT_EQ(Normalize("a​b").text, "a​b");
}

// Custom-pronounced text skips cleaning entirely, whitespace included.
TEST(NormalizeSegment, LeavesCustomPronouncedWhitespaceAlone) {
  EXPECT_EQ(Normalize("  hˈɛ   lo  ", true).text, "  hˈɛ   lo  ");
}

// Adjacent requests are one sample; a space between them keeps them apart.
// Collapsing happens during extraction, so it applies to both segment kinds.
TEST(NormalizeSegment, KeepsMarkersSeparatedBySpace) {
  for (const bool custom : {false, true}) {
    EXPECT_EQ(NormalizeSegment("a◎◎b", false, false, custom, 0).markers.size(), 1u);
    EXPECT_EQ(NormalizeSegment("a◎ ◎b", false, false, custom, 0).markers.size(), 2u);
  }
}

TEST(NormalizeSegment, ExtractsAndCollapsesSampleRequests) {
  const auto result = Normalize("◎a◎b◎◎c◎");
  EXPECT_EQ(result.text, "abc");
  EXPECT_EQ(result.markers, (std::vector<std::size_t>{0, 1, 2, 3}));
}

TEST(NormalizeSegment, RejectsReservedSequenceMarkers) {
  for (const char* text : {"a⏩", "⏪b"}) {
    try {
      NormalizeSegment(text, false, false, false, 2);
      ADD_FAILURE() << "Accepted " << text;
    } catch (const std::invalid_argument& error) {
      EXPECT_STREQ(error.what(),
                   "$.segments[2].text: Reserved sequence-marker character");
    }
  }
}

// U+023A/U+023E lowercase to U+2C65/U+2C66 up in Latin Extended-C. The former
// hand-rolled letter range stopped at U+02AF, so these words split in two and
// reached the encoder as unsupported symbols instead of as a lookup.
TEST(UnknownWords, KeepsLowercasedLatinExtendedBWordsIntact) {
  std::vector<SynthInputSegmentV100> segments(1);
  segments[0].text = NormalizeSegment("Ⱥbc Ⱦd", false, false, false, 0).text;
  EXPECT_EQ(UnknownWords(segments, {}),
            (std::vector<std::string>{"ⱥbc", "ⱦd"}));
}

// Letter classification is the full Unicode L category, so non-Latin alphabetic
// scripts are words rather than runs of unsupported symbols.
//
TEST(UnknownWords, TreatsNonLatinAlphabeticScriptsAsWords) {
  std::vector<SynthInputSegmentV100> segments(1);
  segments[0].text = NormalizeSegment("ΘΕΟΣ Привет", false, false, false, 0).text;
  EXPECT_EQ(UnknownWords(segments, {}),
            (std::vector<std::string>{"θεος", "привет"}));
}

// Combining marks continue a word. Lowercasing İ yields i + U+0307, Hindi vowel
// signs and virama are marks, and so are decomposed accents; none of these may
// split their word.
TEST(UnknownWords, KeepsCombiningMarksInsideWords) {
  std::vector<SynthInputSegmentV100> segments(1);
  segments[0].text =
      NormalizeSegment("İSTANBUL हिन्दी cafe\u0301", false, false, false, 0).text;
  EXPECT_EQ(UnknownWords(segments, {}),
            (std::vector<std::string>{"i\u0307stanbul", "हिन्दी",
                                      "cafe\u0301"}));
}

// A mark with no word before it starts nothing, so it reaches the encoder as a
// symbol instead of becoming a word of pure diacritics.
TEST(UnknownWords, DoesNotStartWordsWithCombiningMarks) {
  std::vector<SynthInputSegmentV100> segments(1);
  segments[0].text = "!\u0301 \u0307a";
  EXPECT_EQ(UnknownWords(segments, {}), (std::vector<std::string>{"a"}));
}

TEST(UnknownWords, ListsEachMissingWordOnceAndSkipsCustomSegments) {
  std::vector<SynthInputSegmentV100> segments(3);
  segments[0].text = "hello world, hello";
  segments[1].text = "xyz";
  segments[1].is_custom_pronounced = true;
  segments[2].text = "don't world";
  const std::unordered_map<std::string, std::string> lookup{{"hello", ""}};
  EXPECT_EQ(UnknownWords(segments, lookup),
            (std::vector<std::string>{"world", "don't"}));
}

class EncodeForCharacterTest : public ::testing::Test {
 protected:
  EncodeForCharacterTest() {
    config.phoneme_ids = {{"h", 1}, {"ə", 2}, {"l", 3},
                          {"o", 4}, {" ", 5},      {"!", 6}};
  }

  std::vector<std::int64_t> Encode(const std::string& text,
                                   const std::vector<std::size_t>& markers = {},
                                   bool custom = false) {
    marker_tokens.clear();
    warnings.clear();
    return EncodeForCharacter(text, markers, lookup, config, custom,
                              &marker_tokens, &warnings);
  }

  CharacterConfig config;
  std::unordered_map<std::string, std::string> lookup{
      {"hello", "həlo"}, {"hi", "hX"}};
  std::vector<std::int64_t> marker_tokens;
  std::vector<std::string> warnings;
};

TEST_F(EncodeForCharacterTest, LooksUpWordsAndPassesOtherSymbolsThrough) {
  EXPECT_EQ(Encode("hello !"), (std::vector<std::int64_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_TRUE(warnings.empty());
}

TEST_F(EncodeForCharacterTest, WarnsAboutSymbolsTheCharacterLacks) {
  EXPECT_EQ(Encode("hi"), (std::vector<std::int64_t>{1}));
  EXPECT_EQ(warnings,
            (std::vector<std::string>{"Skipping unsupported symbol \"X\""}));
}

TEST_F(EncodeForCharacterTest, LooksUpWordsWithCombiningMarksWhole) {
  lookup["he\u0301llo"] = "həlo";
  EXPECT_EQ(Encode("he\u0301llo"), (std::vector<std::int64_t>{1, 2, 3, 4}));
  EXPECT_TRUE(warnings.empty());
}

TEST_F(EncodeForCharacterTest, ThrowsForWordsWithoutPronunciation) {
  EXPECT_THROW(Encode("goodbye"), std::runtime_error);
}

TEST_F(EncodeForCharacterTest, PlacesBoundaryMarkersExactly) {
  Encode("hello !", {0, 5, 6, 7});
  EXPECT_EQ(marker_tokens, (std::vector<std::int64_t>{0, 4, 5, 6}));
}

TEST_F(EncodeForCharacterTest, RescalesMarkersInsideWords) {
  // Grapheme 3 of five maps onto the four-phoneme pronunciation at 12/5.
  Encode("hello", {3});
  EXPECT_EQ(marker_tokens, (std::vector<std::int64_t>{2}));
}

TEST_F(EncodeForCharacterTest, EncodesCustomPronunciationSymbolBySymbol) {
  EXPECT_EQ(Encode("həlo", {2}, true),
            (std::vector<std::int64_t>{1, 2, 3, 4}));
  EXPECT_EQ(marker_tokens, (std::vector<std::int64_t>{2}));
}

}  // namespace
}  // namespace thespeon::inference_detail

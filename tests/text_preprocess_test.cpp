// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/text_preprocess.h"

#include <stdexcept>

#include <gtest/gtest.h>

namespace thespeon::inference_detail {
namespace {

// The English rules, as model-meta-graph builds them. How every character is
// read is checked against the reference runner in metagraph-cpp's tests; these
// check how the runtime applies the rules to segments.
const TextRules& Rules() {
  static const auto rules = TextRules::Load(THESPEON_TEXTPREP_FIXTURE);
  return rules;
}

NormalizedText Normalize(const std::string& text, bool custom = false) {
  return NormalizeSegment(text, false, false, custom, 0, &Rules());
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

TEST(NormalizeSegment, AppliesTheLanguageRules) {
  EXPECT_EQ(Normalize("  Hello \t\n  WORLD  ").text, "hello world");
  EXPECT_EQ(Normalize("Don’t").text, "don't");
  EXPECT_EQ(Normalize("Well-known \"quote\"").text, "well known quote");
  EXPECT_EQ(Normalize("café NAÏVE").text, "café naive");
}

TEST(NormalizeSegment, KeepsOneBoundarySpaceWhenAsked) {
  EXPECT_EQ(NormalizeSegment("  a  ", true, true, false, 0, &Rules()).text,
            " a ");
  EXPECT_EQ(NormalizeSegment("  a  ", true, false, false, 0, &Rules()).text,
            " a");
  EXPECT_EQ(NormalizeSegment("  a  ", false, true, false, 0, &Rules()).text,
            "a ");
}

// Custom-pronounced segments carry a phonetic spelling verbatim, so grapheme
// cleanup is skipped for them, matching the Python service.
TEST(NormalizeSegment, LeavesCustomPronouncedTextAlone) {
  EXPECT_EQ(Normalize("hɛLO’", true).text, "hɛLO’");
  EXPECT_EQ(Normalize("  hˈɛ   lo  ", true).text, "  hˈɛ   lo  ");
  EXPECT_EQ(Normalize("ɑɐʃɒɜˌ", true).text, "ɑɐʃɒɜˌ");
}

// Without rules only the checks that do not depend on language are made.
TEST(NormalizeSegment, LeavesTextAloneWithoutRules) {
  const auto result = NormalizeSegment(" A◎b ", false, false, false, 0, nullptr);
  EXPECT_EQ(result.text, " Ab ");
  EXPECT_EQ(result.markers, (std::vector<std::size_t>{2}));
}

// Adjacent requests are one sample; a space between them keeps them apart.
// Collapsing happens during extraction, so it applies to both segment kinds.
TEST(NormalizeSegment, KeepsMarkersSeparatedBySpace) {
  for (const bool custom : {false, true}) {
    EXPECT_EQ(Normalize("a◎◎b", custom).markers.size(), 1u);
    EXPECT_EQ(Normalize("a◎ ◎b", custom).markers.size(), 2u);
  }
}

TEST(NormalizeSegment, ExtractsAndCollapsesSampleRequests) {
  const auto result = Normalize("◎A◎b◎◎c◎");
  EXPECT_EQ(result.text, "abc");
  EXPECT_EQ(result.markers, (std::vector<std::size_t>{0, 1, 2, 3}));
}

TEST(NormalizeSegment, RejectsReservedSequenceMarkers) {
  for (const auto* rules : {&Rules(), static_cast<const TextRules*>(nullptr)}) {
    for (const bool custom : {false, true}) {
      for (const char* text : {"a⏩", "⏪b"}) {
        try {
          NormalizeSegment(text, false, false, custom, 2, rules);
          ADD_FAILURE() << "Accepted " << text;
        } catch (const std::invalid_argument& error) {
          EXPECT_STREQ(error.what(),
                       "$.segments[2].text: Reserved sequence-marker character");
        }
      }
    }
  }
}

TEST(UnknownWords, ListsEachMissingWordOnceAndSkipsCustomSegments) {
  std::vector<SynthInputSegmentV100> segments(3);
  segments[0].text = "hello world, hello";
  segments[1].text = "xyz";
  segments[1].is_custom_pronounced = true;
  segments[2].text = "don't world";
  const std::unordered_map<std::string, std::string> lookup{{"hello", ""}};
  EXPECT_EQ(UnknownWords(segments, lookup, Rules()),
            (std::vector<std::string>{"world", "don't"}));
}

// Numbers are spoken by the rules, so they are never sent to the phonemizer.
TEST(UnknownWords, LeavesNumbersOut) {
  std::vector<SynthInputSegmentV100> segments(1);
  segments[0].text = "abc12def 21st 'quote'";
  EXPECT_EQ(UnknownWords(segments, {}, Rules()),
            (std::vector<std::string>{"abc", "def", "quote"}));
}

class EncodeForCharacterTest : public ::testing::Test {
 protected:
  EncodeForCharacterTest() {
    std::int64_t id = 1;
    for (const auto symbol : std::u32string(U"həlo !sˈɛvntwfɜːi"))
      config.phoneme_ids[EncodeUtf8(symbol)] = id++;
  }

  std::vector<std::int64_t> Ids(const std::u32string& symbols) const {
    std::vector<std::int64_t> result;
    for (const auto symbol : symbols)
      result.push_back(config.phoneme_ids.at(EncodeUtf8(symbol)));
    return result;
  }

  std::vector<std::int64_t> Encode(const std::string& text,
                                   const std::vector<std::size_t>& markers = {},
                                   bool custom = false) {
    marker_tokens.clear();
    warnings.clear();
    return EncodeForCharacter(text, markers, lookup, Rules(), config, custom,
                              &marker_tokens, &warnings);
  }

  CharacterConfig config;
  std::unordered_map<std::string, std::string> lookup{
      {"hello", "həlo"}, {"hi", "hX"}};
  std::vector<std::int64_t> marker_tokens;
  std::vector<std::string> warnings;
};

TEST_F(EncodeForCharacterTest, LooksUpWordsAndPassesOtherSymbolsThrough) {
  EXPECT_EQ(Encode("hello !"), Ids(U"həlo !"));
  EXPECT_TRUE(warnings.empty());
}

TEST_F(EncodeForCharacterTest, WarnsAboutSymbolsTheCharacterLacks) {
  EXPECT_EQ(Encode("hi"), Ids(U"h"));
  EXPECT_EQ(warnings,
            (std::vector<std::string>{"Skipping unsupported symbol \"X\""}));
}

TEST_F(EncodeForCharacterTest, ThrowsForWordsWithoutPronunciation) {
  EXPECT_THROW(Encode("goodbye"), std::runtime_error);
}

TEST_F(EncodeForCharacterTest, SpeaksNumbersOut) {
  EXPECT_EQ(Encode("7 hello"), Ids(U"sˈɛvən həlo"));
  EXPECT_EQ(Encode("21st"), Ids(U"twˈɛnti fˈɜːst"));
  EXPECT_TRUE(warnings.empty());
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

TEST_F(EncodeForCharacterTest, RescalesMarkersInsideNumbers) {
  // Digit 1 of "21st" maps onto its 14 phonemes at 14/4.
  Encode("21st", {1});
  EXPECT_EQ(marker_tokens, (std::vector<std::int64_t>{4}));
}

TEST_F(EncodeForCharacterTest, EncodesCustomPronunciationSymbolBySymbol) {
  EXPECT_EQ(Encode("həlo 7", {2}, true), Ids(U"həlo "));
  EXPECT_EQ(warnings,
            (std::vector<std::string>{"Skipping unsupported symbol \"7\""}));
  EXPECT_EQ(marker_tokens, (std::vector<std::int64_t>{2}));
}

}  // namespace
}  // namespace thespeon::inference_detail

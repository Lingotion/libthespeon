// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/input/thespeon_input.h"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace thespeon {
namespace {

const SynthInputV100& Parse(const std::string& json, ThespeonInput& storage) {
  storage = ParseThespeonInput(json);
  return std::get<SynthInputV100>(storage);
}

// Wraps segments and extra top-level members in an otherwise valid document.
std::string Document(const std::string& segments = R"([{"text": "Hello"}])",
                     const std::string& extra = R"("moduleIdentifier": "abc")") {
  return R"({"version": "1.0.0", "defaultLanguage": {"iso639_2": "eng"},)" +
         extra + R"(, "segments": )" + segments + "}";
}

// The JSON-path message the parser raised, or empty when it accepted the input.
std::string ParseError(const std::string& json) {
  try {
    ParseThespeonInput(json);
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return {};
}

TEST(ParseThespeonInput, ParsesMinimalIdentifierDocument) {
  ThespeonInput storage;
  const auto& input = Parse(Document(), storage);
  EXPECT_EQ(input.module_identifier, "abc");
  EXPECT_EQ(input.default_language.iso639_2, "eng");
  ASSERT_EQ(input.segments.size(), 1u);
  EXPECT_EQ(input.segments[0].text, "Hello");
  EXPECT_FALSE(input.segments[0].is_custom_pronounced);
  EXPECT_FALSE(input.default_emotion.has_value());
}

TEST(ParseThespeonInput, ParsesCharacterNameWithNarrowing) {
  ThespeonInput storage;
  const auto& input = Parse(
      Document(R"([{"text": "Hi"}])",
               R"("characterName": " Ada ", "moduleType": "L", "moduleVersion": "v2.1.0")"),
      storage);
  EXPECT_EQ(input.character_name, "Ada");
  EXPECT_EQ(input.module_type, "L");
  EXPECT_EQ(input.module_version, (Version{2, 1, 0}));
}

TEST(ParseThespeonInput, RecordsFieldsIgnoredBesideAnIdentifier) {
  ThespeonInput storage;
  const auto& input = Parse(
      Document(R"([{"text": "Hi"}])",
               R"("moduleIdentifier": "abc", "characterName": "Ada", "moduleType": "bogus")"),
      storage);
  EXPECT_EQ(input.ignored_fields,
            (std::vector<std::string>{"characterName", "moduleType"}));
  EXPECT_TRUE(input.character_name.empty());
}

TEST(ParseThespeonInput, LegacyEmotionPinsOnlyMissingBoundaries) {
  ThespeonInput storage;
  const auto& input = Parse(
      Document(R"([{"text": "Hi", "emotion": "Joy", "endEmotion": {"Anger": 0.5}}])"),
      storage);
  const auto& segment = input.segments[0];
  EXPECT_EQ(segment.start_emotion, (EmotionWeights{{"Joy", 1.0}}));
  EXPECT_EQ(segment.end_emotion, (EmotionWeights{{"Anger", 0.5}}));
}

TEST(ParseThespeonInput, ParsesSegmentProsodyAndLanguage) {
  ThespeonInput storage;
  const auto& input = Parse(
      Document(R"([{"text": "hɛˈloʊ", "isCustomPronounced": true,
                    "startSpeed": 0.5, "endLoudness": 2,
                    "language": {"iso639_2": "eng", "iso3166_1": "GB"}}])"),
      storage);
  const auto& segment = input.segments[0];
  EXPECT_TRUE(segment.is_custom_pronounced);
  EXPECT_EQ(segment.start_speed, 0.5);
  EXPECT_FALSE(segment.end_speed.has_value());
  EXPECT_EQ(segment.end_loudness, 2.0);
  ASSERT_TRUE(segment.language.has_value());
  EXPECT_EQ(segment.language->iso3166_1, "GB");
}

TEST(ParseThespeonInput, ReportsJsonPathsForInvalidDocuments) {
  struct Case {
    std::string json;
    std::string message;
  };
  const Case cases[] = {
      {"[]", "$: Expected object"},
      {R"({})", "$.version: Required"},
      {R"({"version": 1})", "$.version: Expected string"},
      {R"({"version": "2.0.0"})", "$.version: Unsupported version"},
      {Document("[]"), "$.segments: Expected at least one segment"},
      {Document(R"({"text": "Hi"})"), "$.segments: Expected array"},
      {Document(R"([{"text": "   "}])"), "$.segments[0].text: Required"},
      {Document(R"([{"text": "a"}, {"text": 3}])"),
       "$.segments[1].text: Expected string"},
      {Document(R"([{"text": "a", "startSpeed": 0}])"),
       "$.segments[0].startSpeed: Expected positive number"},
      {Document(R"([{"text": "a", "endLoudness": "loud"}])"),
       "$.segments[0].endLoudness: Expected number"},
      {Document(R"([{"text": "a", "startEmotion": {"Joy": -1}}])"),
       "$.segments[0].startEmotion.Joy: Expected nonnegative number"},
      {Document(R"([{"text": "a", "isCustomPronounced": 1}])"),
       "$.segments[0].isCustomPronounced: Expected boolean"},
      {Document(R"([{"text": "a"}])", R"("moduleIdentifier": " ")"),
       "$.moduleIdentifier: Required"},
      {Document(R"([{"text": "a"}])", R"("defaultEmotion": "Joy")"),
       "$: Expected moduleIdentifier or characterName"},
      {Document(R"([{"text": "a"}])", R"("characterName": "Ada", "moduleType": "XXL")"),
       "$.moduleType: Expected one of XS, S, M, L, XL"},
      {Document(R"([{"text": "a"}])", R"("characterName": "Ada", "moduleVersion": "3")"),
       "$.moduleVersion: Expected a version such as 3.0.1"},
      {R"({"version": "1.0.0", "moduleIdentifier": "abc", "segments": [{"text": "a"}]})",
       "$.defaultLanguage: Required"},
  };
  for (const auto& test_case : cases) {
    EXPECT_EQ(ParseError(test_case.json), test_case.message) << test_case.json;
  }
}

TEST(ParseThespeonInput, WrapsMalformedJsonAsInvalidArgument) {
  EXPECT_EQ(ParseError("{not json").rfind("Invalid synthesis JSON: ", 0), 0u);
}

TEST(ConcatenateSegmentText, JoinsSegmentsWithoutSeparators) {
  const auto input =
      ParseThespeonInput(Document(R"([{"text": "One "}, {"text": "two."}])"));
  EXPECT_EQ(ConcatenateSegmentText(input), "One two.");
}

TEST(SelectedCharacter, CopiesSelectionFields) {
  const auto input = ParseThespeonInput(Document(
      R"([{"text": "a"}])", R"("characterName": "Ada", "moduleType": "S")"));
  const auto selection = SelectedCharacter(input);
  EXPECT_EQ(selection.character_name, "Ada");
  EXPECT_EQ(selection.module_type, "S");
  EXPECT_TRUE(selection.module_identifier.empty());
  EXPECT_FALSE(selection.module_version.IsValid());
}

TEST(SegmentLanguages, PrefersTheSegmentLanguageOverTheDefault) {
  const auto input = ParseThespeonInput(Document(
      R"([{"text": "Hej", "language": {"iso639_2": "SWE"}}, {"text": "Hi"}])"));
  EXPECT_EQ(SegmentLanguages(input),
            (std::vector<std::string>{"swe", "eng"}));
}

TEST(SegmentLanguages, FallsBackToEnglishWithoutADefault) {
  SynthInputV100 input;
  input.segments.resize(2);
  input.segments[1].language = ModuleLanguage{"fin"};
  EXPECT_EQ(SegmentLanguages(input),
            (std::vector<std::string>{"eng", "fin"}));
}

}  // namespace
}  // namespace thespeon

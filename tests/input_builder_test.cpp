// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/input/input_builder.h"

#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace thespeon {
namespace {

using nlohmann::json;

json Built(const InputBuilder& builder) {
  return json::parse(builder.ToJson());
}

std::string ToJsonError(const InputBuilder& builder) {
  try {
    builder.ToJson();
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return {};
}

InputBuilder WithOneSegment() {
  InputBuilder builder;
  builder.SetCharacterModule("abc");
  builder.AddSegment("Hello");
  return builder;
}

TEST(InputBuilder, BuildsEveryField) {
  InputBuilder builder;
  builder.SetCharacterModule("61469f1e34796be9a253e59f802bf498");
  ModuleLanguage us;
  us.iso639_2 = "eng";
  us.iso3166_1 = "US";
  builder.SetDefaultLanguage(us);
  builder.SetDefaultEmotion("Interest");

  const auto first = builder.AddSegment("Let me tell you.");
  builder.SetSegmentEmotion(first, "Interest");
  builder.SetSegmentSpeed(first, Endpoint::Start, 1.0);
  builder.SetSegmentLoudness(first, Endpoint::Start, 1.0);

  const auto second = builder.AddSegment("It seemed normal.");
  builder.AddSegmentEmotion(second, Endpoint::Start, "Interest", 0.7);
  builder.AddSegmentEmotion(second, Endpoint::Start, "Sadness", 0.3);
  builder.AddSegmentEmotion(second, Endpoint::End, "Fear", 0.4);
  builder.SetSegmentSpeed(second, Endpoint::End, 0.75);
  builder.SetSegmentLoudness(second, Endpoint::End, 0.6);

  const auto third = builder.AddSegment("h\xC9\x99\xCB\x88lo\xCA\x8A");
  builder.SetCustomPronounced(third, true);
  ModuleLanguage swedish;
  swedish.iso639_2 = "swe";
  swedish.custom_dialect = "Gotland";
  builder.SetSegmentLanguage(third, swedish);

  EXPECT_EQ(second, 1u);
  EXPECT_EQ(Built(builder), json::parse(R"({
    "version": "1.0.0",
    "moduleIdentifier": "61469f1e34796be9a253e59f802bf498",
    "defaultLanguage": {"iso639_2": "eng", "iso3166_1": "US"},
    "defaultEmotion": "Interest",
    "segments": [
      {"text": "Let me tell you.", "emotion": "Interest",
       "startSpeed": 1.0, "startLoudness": 1.0},
      {"text": "It seemed normal.",
       "startEmotion": {"Interest": 0.7, "Sadness": 0.3},
       "endEmotion": {"Fear": 0.4}, "endSpeed": 0.75, "endLoudness": 0.6},
      {"text": "həˈloʊ", "isCustomPronounced": true,
       "language": {"iso639_2": "swe", "customDialect": "Gotland"}}
    ]
  })"));
}

TEST(InputBuilder, FromJsonKeepsTheDocumentEditable) {
  auto builder = InputBuilder::FromJson(
      R"({"version": "1.0.0", "moduleIdentifier": "abc",
          "defaultLanguage": {"iso639_2": "eng"},
          "segments": [{"text": "Hello", "startSpeed": 1.5}]})");
  EXPECT_EQ(builder.AddSegment("Again"), 1u);
  builder.SetSegmentEmotion(0, "Joy");
  EXPECT_EQ(Built(builder)["segments"], json::parse(R"([
    {"text": "Hello", "startSpeed": 1.5, "emotion": "Joy"},
    {"text": "Again"}
  ])"));
  const auto input = std::get<SynthInputV100>(builder.Build());
  EXPECT_EQ(input.segments.size(), 2u);
}

TEST(InputBuilder, FromJsonRejectsInvalidDocuments) {
  EXPECT_THROW(InputBuilder::FromJson("{"), std::invalid_argument);
  EXPECT_THROW(InputBuilder::FromJson(R"({"version": "1.0.0"})"),
               std::invalid_argument);
}

TEST(InputBuilder, ModuleAndCharacterReplaceEachOther) {
  auto builder = WithOneSegment();
  builder.SetCharacter("Ada", "L", "3.0.1");
  auto document = Built(builder);
  EXPECT_FALSE(document.contains("moduleIdentifier"));
  EXPECT_EQ(document["characterName"], "Ada");
  EXPECT_EQ(document["moduleType"], "L");
  EXPECT_EQ(document["moduleVersion"], "3.0.1");

  builder.SetCharacter("Ada", std::nullopt, std::nullopt);
  document = Built(builder);
  EXPECT_FALSE(document.contains("moduleType"));
  EXPECT_FALSE(document.contains("moduleVersion"));

  builder.SetCharacterModule("abc");
  document = Built(builder);
  EXPECT_EQ(document["moduleIdentifier"], "abc");
  EXPECT_FALSE(document.contains("characterName"));
}

TEST(InputBuilder, NullUnsets) {
  auto builder = WithOneSegment();
  ModuleLanguage language;
  language.iso639_2 = "swe";
  builder.SetDefaultEmotion("Joy");
  builder.SetSegmentEmotion(0, "Joy");
  builder.SetSegmentLanguage(0, language);
  builder.SetDefaultEmotion(std::nullopt);
  builder.SetSegmentEmotion(0, std::nullopt);
  builder.SetSegmentLanguage(0, std::nullopt);
  EXPECT_EQ(Built(builder)["segments"][0], json::parse(R"({"text": "Hello"})"));
  EXPECT_FALSE(Built(builder).contains("defaultEmotion"));
}

TEST(InputBuilder, SameEmotionNameOverwrites) {
  auto builder = WithOneSegment();
  builder.AddSegmentEmotion(0, Endpoint::End, "Joy", 0.2);
  builder.AddSegmentEmotion(0, Endpoint::End, "Joy", 0.9);
  EXPECT_EQ(Built(builder)["segments"][0]["endEmotion"],
            json::parse(R"({"Joy": 0.9})"));
}

TEST(InputBuilder, RejectsBadArguments) {
  constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
  constexpr auto inf = std::numeric_limits<double>::infinity();
  auto builder = WithOneSegment();
  EXPECT_THROW(builder.SetSegmentSpeed(1, Endpoint::Start, 1.0),
               std::invalid_argument);
  EXPECT_THROW(builder.SetSegmentSpeed(0, Endpoint::Start, nan),
               std::invalid_argument);
  EXPECT_THROW(builder.SetSegmentLoudness(0, Endpoint::End, inf),
               std::invalid_argument);
  EXPECT_THROW(builder.AddSegmentEmotion(0, Endpoint::End, "Joy", inf),
               std::invalid_argument);
}

TEST(InputBuilder, ToJsonReportsSchemaErrors) {
  InputBuilder empty;
  empty.SetCharacterModule("abc");
  EXPECT_EQ(ToJsonError(empty), "$.segments: Expected at least one segment");

  InputBuilder blank;
  blank.SetCharacterModule("abc");
  blank.AddSegment("  ");
  EXPECT_EQ(ToJsonError(blank), "$.segments[0].text: Required");

  auto slow = WithOneSegment();
  slow.SetSegmentSpeed(0, Endpoint::Start, 0.0);
  EXPECT_EQ(ToJsonError(slow),
            "$.segments[0].startSpeed: Expected positive number");

  InputBuilder anonymous;
  anonymous.AddSegment("Hello");
  EXPECT_EQ(ToJsonError(anonymous),
            "$: Expected moduleIdentifier or characterName");
}

}  // namespace
}  // namespace thespeon

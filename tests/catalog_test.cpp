// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/module/catalog.h"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "test_support.h"

namespace thespeon {
namespace {

using nlohmann::json;

json VersionJson(const Version& version) {
  return {{"major", version.major},
          {"minor", version.minor},
          {"patch", version.patch}};
}

class CatalogTest : public ::testing::Test {
 protected:
  std::filesystem::path Config(const std::string& file) const {
    return directory.path() / "configs" / file;
  }

  void AddCharacter(const std::string& file, const std::string& identifier,
                    const std::string& name, const std::string& quality,
                    const Version& version,
                    const std::string& required_language_base = {}) {
    json config = {{"type", "lara"},
                   {"module_identifier", identifier},
                   {"version", VersionJson(version)},
                   {"character", {{"charactername", name}}},
                   {"tags", {{"module_type", quality}}},
                   {"languages", {{{"iso639_2", "ENG"}, {"iso3166_1", "US"}}}}};
    if (!required_language_base.empty()) {
      config["phonemizer_setup"] = {
          {"modules",
           {{{"iso639_2", "eng"}, {"base_module_id", required_language_base}}}}};
    }
    test::WriteFile(Config(file), config.dump());
  }

  void AddLanguage(const std::string& file, const std::string& identifier,
                   const std::string& base, const std::string& iso639_2 = "eng") {
    const json config = {{"type", "phonemizer"},
                         {"module_identifier", identifier},
                         {"base_module_id", base},
                         {"name", "Phonemizer " + identifier},
                         {"version", VersionJson({1, 0, 0})},
                         {"languages", {{{"iso639_2", iso639_2}}}}};
    test::WriteFile(Config(file), config.dump());
  }

  Catalog Load() const { return Catalog::Load(directory.path()); }

  // The message the selection threw, or empty when it succeeded.
  template <typename Select>
  static std::string SelectionError(Select&& select) {
    try {
      select();
    } catch (const std::runtime_error& error) {
      return error.what();
    }
    return {};
  }

  test::TempDirectory directory;
};

TEST_F(CatalogTest, LoadsNothingWithoutAConfigsDirectory) {
  const auto catalog = Load();
  EXPECT_TRUE(catalog.character_modules().empty());
  EXPECT_TRUE(catalog.language_modules().empty());
  EXPECT_TRUE(catalog.problems().empty());
}

TEST_F(CatalogTest, LoadsModulesSortedAndReportsUnreadableConfigs) {
  AddCharacter("1.json", "c-bo", "Bo", "mid", {1, 0, 0});
  AddCharacter("2.json", "c-ada", "Ada", "mid", {1, 0, 0});
  AddLanguage("3.json", "l-2", "base");
  AddLanguage("4.json", "l-1", "base");
  test::WriteFile(Config("broken.json"), "{");
  test::WriteFile(Config("other.json"), R"({"type": "something"})");
  test::WriteFile(Config("notes.txt"), "{");

  const auto catalog = Load();
  ASSERT_EQ(catalog.character_modules().size(), 2u);
  EXPECT_EQ(catalog.character_modules()[0].character_name, "Ada");
  EXPECT_EQ(catalog.character_modules()[0].languages[0].iso639_2, "eng");
  EXPECT_EQ(catalog.character_modules()[1].character_name, "Bo");
  ASSERT_EQ(catalog.language_modules().size(), 2u);
  EXPECT_EQ(catalog.language_modules()[0].identifier, "l-1");
  ASSERT_EQ(catalog.problems().size(), 1u);
  EXPECT_EQ(catalog.problems()[0].rfind("broken.json: ", 0), 0u);
}

TEST_F(CatalogTest, SelectsTheLargestThenNewestCharacter) {
  AddCharacter("1.json", "small-new", "Ada", "low", {9, 0, 0});
  AddCharacter("2.json", "large-old", "Ada", "high", {1, 0, 0});
  AddCharacter("3.json", "large-new", "Ada", "high", {1, 2, 0});
  const auto catalog = Load();
  EXPECT_EQ(catalog.SelectCharacter("ada").identifier, "large-new");
  EXPECT_EQ(catalog.SelectCharacter("Ada", "S").identifier, "small-new");
  EXPECT_EQ(catalog.SelectCharacter("Ada", "", {1, 0, 0}).identifier,
            "large-old");
}

TEST_F(CatalogTest, ExplainsSelectionsThatMatchNothing) {
  AddCharacter("1.json", "a", "Ada", "high", {1, 0, 0});
  const auto catalog = Load();
  EXPECT_EQ(SelectionError([&] { catalog.SelectCharacter("Bo"); }),
            "No character module named \"Bo\" is installed");
  EXPECT_EQ(SelectionError([&] { catalog.SelectCharacter("Ada", "XL"); }),
            "No XL character module is installed for \"Ada\"; installed: L v1.0.0");
  EXPECT_EQ(
      SelectionError([&] { catalog.SelectCharacter("Ada", "", {2, 0, 0}); }),
      "No version 2.0.0 character module is installed for \"Ada\"; "
      "installed: L v1.0.0");
}

TEST_F(CatalogTest, RefusesToGuessBetweenIndistinguishableModules) {
  AddCharacter("1.json", "first", "Ada", "high", {1, 0, 0});
  AddCharacter("2.json", "second", "Ada", "high", {1, 0, 0});
  const auto catalog = Load();
  EXPECT_EQ(SelectionError([&] { catalog.SelectCharacter("Ada"); }),
            "More than one character module named \"Ada\" is installed; name "
            "it by module identifier first or second");
}

TEST_F(CatalogTest, SelectsByModuleIdentifier) {
  AddCharacter("1.json", "first", "Ada", "high", {1, 0, 0});
  AddCharacter("2.json", "second", "Ada", "high", {1, 0, 0});
  const auto catalog = Load();
  EXPECT_EQ(catalog.SelectCharacter("SECOND").identifier, "second");
  EXPECT_EQ(catalog.SelectCharacter("first", "L", {1, 0, 0}).identifier,
            "first");
  EXPECT_EQ(SelectionError([&] { catalog.SelectCharacter("first", "XL"); }),
            "No XL character module is installed for module identifier "
            "first; installed: L v1.0.0");
}

TEST_F(CatalogTest, FindsByIdentifierCaseInsensitively) {
  AddCharacter("1.json", "abc", "Ada", "high", {1, 0, 0});
  AddLanguage("2.json", "def", "base");
  const auto catalog = Load();
  ASSERT_NE(catalog.FindCharacterByIdentifier("ABC"), nullptr);
  EXPECT_EQ(catalog.FindCharacterByIdentifier("ABC")->character_name, "Ada");
  ASSERT_NE(catalog.FindLanguageByIdentifier("DEF"), nullptr);
  EXPECT_EQ(catalog.FindLanguageByIdentifier("DEF")->identifier, "def");
  EXPECT_EQ(catalog.FindCharacterByIdentifier("def"), nullptr);
  EXPECT_EQ(catalog.FindLanguageByIdentifier("abc"), nullptr);
}

TEST_F(CatalogTest, PrefersTheLanguageModuleTheCharacterRequires) {
  AddCharacter("1.json", "c", "Ada", "high", {1, 0, 0}, "wanted");
  AddLanguage("2.json", "a-other", "other");
  AddLanguage("3.json", "b-wanted", "wanted");
  const auto catalog = Load();
  std::string language;
  EXPECT_EQ(catalog
                .CompatibleLanguageModule(catalog.character_modules()[0],
                                          language)
                .identifier,
            "b-wanted");
  EXPECT_EQ(language, "eng");
}

TEST_F(CatalogTest, FallsBackToAnyEnglishLanguageModule) {
  AddCharacter("1.json", "c", "Ada", "high", {1, 0, 0}, "missing");
  AddLanguage("2.json", "a-swedish", "base", "swe");
  AddLanguage("3.json", "b-english", "base");
  const auto catalog = Load();
  std::string language;
  EXPECT_EQ(catalog
                .CompatibleLanguageModule(catalog.character_modules()[0],
                                          language)
                .identifier,
            "b-english");
}

TEST_F(CatalogTest, ThrowsWithoutAnEnglishLanguageModule) {
  AddCharacter("1.json", "c", "Ada", "high", {1, 0, 0});
  AddLanguage("2.json", "l", "base", "swe");
  const auto catalog = Load();
  std::string language;
  EXPECT_THROW(
      catalog.CompatibleLanguageModule(catalog.character_modules()[0], language),
      std::runtime_error);
}

TEST_F(CatalogTest, SelectsTheRequestedLanguage) {
  AddCharacter("1.json", "c", "Ada", "high", {1, 0, 0});
  AddLanguage("2.json", "a-english", "base");
  AddLanguage("3.json", "b-swedish", "base", "swe");
  const auto catalog = Load();
  std::string language;
  EXPECT_EQ(catalog
                .CompatibleLanguageModule(catalog.character_modules()[0],
                                          language, "swe")
                .identifier,
            "b-swedish");
  EXPECT_EQ(language, "swe");
}

TEST_F(CatalogTest, ThrowsWithoutTheRequestedLanguage) {
  AddCharacter("1.json", "c", "Ada", "high", {1, 0, 0});
  AddLanguage("2.json", "l", "base");
  const auto catalog = Load();
  std::string language;
  EXPECT_THROW(catalog.CompatibleLanguageModule(catalog.character_modules()[0],
                                                language, "swe"),
               std::runtime_error);
}

TEST_F(CatalogTest, WithoutDropsOnlyTheNamedConfig) {
  AddCharacter("1.json", "a", "Ada", "high", {1, 0, 0});
  AddCharacter("2.json", "b", "Bo", "high", {1, 0, 0});
  AddLanguage("3.json", "l", "base");
  const auto remaining = Load().Without(Config("1.json"));
  ASSERT_EQ(remaining.character_modules().size(), 1u);
  EXPECT_EQ(remaining.character_modules()[0].identifier, "b");
  EXPECT_EQ(remaining.language_modules().size(), 1u);
}

TEST_F(CatalogTest, SerializesTheListingAsJson) {
  AddCharacter("1.json", "a", "Ada", "ultrahigh", {3, 0, 1});
  AddLanguage("2.json", "l", "base");
  const auto listing = json::parse(CatalogJson(Load()));

  ASSERT_EQ(listing["characterModules"].size(), 1u);
  const auto& character = listing["characterModules"][0];
  EXPECT_EQ(character["identifier"], "a");
  EXPECT_EQ(character["name"], "Ada");
  EXPECT_EQ(character["moduleType"], "XL");
  EXPECT_EQ(character["version"], "3.0.1");
  EXPECT_EQ(character["languages"][0]["country"], "US");

  ASSERT_EQ(listing["languageModules"].size(), 1u);
  EXPECT_EQ(listing["languageModules"][0]["languages"][0]["iso639_2"], "eng");
  EXPECT_TRUE(listing["problems"].empty());
}

TEST_F(CatalogTest, ListsTheEmotionsSynthesisAccepts) {
  AddCharacter("1.json", "a", "Ada", "high", {1, 0, 0});
  auto config = json::parse(test::ReadFile(Config("1.json")));
  config["emotionsets"] = {
      {{"emotionsetname", "Joy"},
       {"emotionsetkey", 7},
       {"emotionsetguide", "Feeling good."}},
      {{"emotionsetname", "Unused"}, {"emotionsetkey", 0}},
      {{"emotionsetkey", 3}},
      {{"emotionsetname", "Interest"}, {"emotionsetkey", 24}}};
  test::WriteFile(Config("1.json"), config.dump());
  const auto catalog = Load();

  const auto& emotions = catalog.character_modules()[0].emotions;
  ASSERT_EQ(emotions.size(), 2u);
  EXPECT_EQ(emotions[0].name, "Joy");
  EXPECT_EQ(emotions[0].guide, "Feeling good.");
  EXPECT_EQ(emotions[1].name, "Interest");
  EXPECT_EQ(emotions[1].guide, "");
  EXPECT_EQ(FindEmotion(catalog.character_modules()[0], "jOY"), &emotions[0]);
  EXPECT_EQ(FindEmotion(catalog.character_modules()[0], "Unused"), nullptr);

  const auto listed =
      json::parse(CatalogJson(catalog))["characterModules"][0]["emotions"];
  EXPECT_EQ(listed, json::parse(EmotionsJson(catalog.character_modules()[0])));
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0]["name"], "Joy");
  EXPECT_EQ(listed[0]["guide"], "Feeling good.");
}

TEST(ParseNarrowing, AcceptsEmptyOrValidValues) {
  EXPECT_FALSE(ParseNarrowing("", "").IsValid());
  EXPECT_EQ(ParseNarrowing("m", "v1.2.3"), (Version{1, 2, 3}));
}

TEST(ParseNarrowing, RejectsInvalidValues) {
  EXPECT_THROW(ParseNarrowing("huge", ""), std::runtime_error);
  EXPECT_THROW(ParseNarrowing("", "1.2"), std::runtime_error);
}

}  // namespace
}  // namespace thespeon

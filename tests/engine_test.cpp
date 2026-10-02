// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/engine.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "test_support.h"

namespace thespeon {
namespace {

namespace fs = std::filesystem;

// Real synthesis needs installed modules, which the repository does not ship.
fs::path TestDataDirectory() {
  if (const char* path = std::getenv("THESPEON_TEST_DATA_DIR")) return path;
  return DefaultDataDirectory();
}

std::optional<CharacterModule> UsableCharacter(const Catalog& catalog) {
  for (const auto& character : catalog.character_modules()) {
    if (character.identifier.empty()) continue;
    try {
      std::string language;
      catalog.CompatibleLanguageModule(character, language);
      return character;
    } catch (const std::exception&) {
    }
  }
  return std::nullopt;
}

class ValidateInputTest : public ::testing::Test {
 protected:
  ValidateInputTest() {
    using nlohmann::json;
    const json version = {{"major", 1}, {"minor", 0}, {"patch", 0}};
    test::WriteFile(
        data.path() / "configs" / "1.json",
        json{{"type", "lara"},
             {"module_identifier", "c-ada"},
             {"version", version},
             {"character", {{"charactername", "Ada"}}},
             {"tags", {{"module_type", "high"}}},
             {"languages", {{{"iso639_2", "eng"}}}},
             {"emotionsets",
              {{{"emotionsetname", "Joy"}, {"emotionsetkey", 7}},
               {{"emotionsetname", "Interest"}, {"emotionsetkey", 24}}}}}
            .dump());
    test::WriteFile(data.path() / "configs" / "2.json",
                    json{{"type", "phonemizer"},
                         {"module_identifier", "l-eng"},
                         {"name", "English"},
                         {"version", version},
                         {"languages", {{{"iso639_2", "eng"}}}}}
                        .dump());
  }

  SynthInputV100 Input() const {
    SynthInputV100 input;
    input.module_identifier = "c-ada";
    input.segments.emplace_back().text = "Hello there.";
    return input;
  }

  // The message validation threw, or empty when it passed.
  std::string Error(const SynthInputV100& input) const {
    try {
      ValidateInput(Catalog::Load(data.path()), input);
    } catch (const std::invalid_argument& error) {
      return error.what();
    }
    return {};
  }

  test::TempDirectory data;
};

TEST_F(ValidateInputTest, AcceptsListedEmotionsInAnyCase) {
  auto input = Input();
  input.default_emotion = "interest";
  input.segments[0].emotion = "JOY";
  input.segments[0].start_emotion = EmotionWeights{{"joy", 0.5}, {"None", 1}};
  EXPECT_EQ(ValidateInput(Catalog::Load(data.path()), input).identifier,
            "c-ada");
}

TEST_F(ValidateInputTest, NamesTheUnknownEmotionAndWhereItIs) {
  auto input = Input();
  input.segments[0].end_emotion = EmotionWeights{{"Joyy", 1}};
  EXPECT_EQ(Error(input),
            "$.segments[0].endEmotion: Character \"Ada\" has no emotion "
            "\"Joyy\"");
  input = Input();
  input.default_emotion = "Rage";
  EXPECT_EQ(Error(input),
            "$.defaultEmotion: Character \"Ada\" has no emotion \"Rage\"");
}

TEST_F(ValidateInputTest, RejectsAnUnsupportedLanguage) {
  auto input = Input();
  input.default_language.iso639_2 = "swe";
  EXPECT_THROW(ValidateInput(Catalog::Load(data.path()), input),
               std::runtime_error);
}

TEST(EngineTest, PreloadSynthesizeAndUnloadByIdentifier) {
  const auto data_directory = TestDataDirectory();
  if (!fs::is_directory(data_directory))
    GTEST_SKIP() << "No model data directory at " << data_directory;
  Engine engine(data_directory);
  const auto character = UsableCharacter(engine.catalog());
  if (!character)
    GTEST_SKIP() << "No usable character module in " << data_directory;

  SynthInputV100 input;
  input.module_identifier = character->identifier;
  input.default_language.iso639_2 = "eng";
  input.segments.emplace_back().text = "A short test sentence.";

  ASSERT_EQ(engine.ResidentBytes(), 0u);
  EXPECT_FALSE(engine.IsLoaded(character->identifier));
  EXPECT_FALSE(engine.IsLoaded(input));
  EXPECT_TRUE(engine.GetAllLoaded().character_modules.empty());
  EXPECT_TRUE(engine.GetAllLoaded().language_modules.empty());

  engine.Preload(character->identifier);
  const auto preloaded = engine.ResidentBytes();
  EXPECT_GT(preloaded, 0u);
  EXPECT_TRUE(engine.IsLoaded(character->identifier));
  EXPECT_TRUE(engine.IsLoaded(input));
  const auto loaded = engine.GetAllLoaded();
  ASSERT_EQ(loaded.character_modules.size(), 1u);
  EXPECT_EQ(loaded.character_modules[0].identifier, character->identifier);
  ASSERT_EQ(loaded.language_modules.size(), 1u);
  EXPECT_TRUE(engine.IsLoaded(loaded.language_modules[0].identifier));

  std::size_t samples = 0;
  bool received_final = false;
  engine.SynthesizeWithCallbacks(
      input, [&](metagraph::CallbackPacket packet) {
        if (packet.type != metagraph::CallbackType::Audio) return;
        samples += std::get<std::vector<float>>(packet.payload).size();
        const auto found = packet.metadata.find("is_final");
        if (found != packet.metadata.end())
          received_final = received_final || std::get<bool>(found->second);
      });
  EXPECT_GT(samples, 0u);
  EXPECT_TRUE(received_final);
  EXPECT_EQ(engine.ResidentBytes(), preloaded);

  engine.Unload(character->identifier);
  EXPECT_EQ(engine.ResidentBytes(), 0u);
  EXPECT_FALSE(engine.IsLoaded(character->identifier));
  EXPECT_FALSE(engine.IsLoaded(input));
  EXPECT_FALSE(engine.IsLoaded(loaded.language_modules[0].identifier));
  EXPECT_TRUE(engine.GetAllLoaded().character_modules.empty());
  EXPECT_TRUE(engine.GetAllLoaded().language_modules.empty());
}

TEST(EngineTest, ListsLoadedWithoutFailingOnUnreadableModule) {
  test::TempDirectory directory;
  // No characterkey and no metagraph, so its config cannot be loaded.
  const nlohmann::json config = {
      {"type", "lara"},
      {"module_identifier", "abc"},
      {"version", {{"major", 1}, {"minor", 0}, {"patch", 0}}},
      {"character", {{"charactername", "Ada"}}},
      {"tags", {{"module_type", "M"}}}};
  test::WriteFile(directory.path() / "configs" / "1.json", config.dump());
  Engine engine(directory.path());
  ASSERT_EQ(engine.catalog().character_modules().size(), 1u);

  const auto loaded = engine.GetAllLoaded();
  EXPECT_TRUE(loaded.character_modules.empty());
  EXPECT_TRUE(loaded.language_modules.empty());
  EXPECT_THROW(engine.IsLoaded("abc"), std::exception);
  EXPECT_THROW(engine.IsLoaded("missing"), std::runtime_error);
}

std::size_t SynthesizedSamples(Engine& engine, const CharacterModule& character) {
  SynthInputV100 input;
  input.module_identifier = character.identifier;
  input.default_language.iso639_2 = "eng";
  input.segments.emplace_back().text = "A short test sentence.";
  std::size_t samples = 0;
  engine.SynthesizeWithCallbacks(input, [&](metagraph::CallbackPacket packet) {
    if (packet.type == metagraph::CallbackType::Audio)
      samples += std::get<std::vector<float>>(packet.payload).size();
  });
  return samples;
}

TEST(EngineTest, SynthesizesWithSessionOptionsThatLockWhileInUse) {
  const auto data_directory = TestDataDirectory();
  if (!fs::is_directory(data_directory))
    GTEST_SKIP() << "No model data directory at " << data_directory;
  EngineOptions config;
  config.intra_op_threads = 2;
  config.allow_spinning = true;
  config.low_memory = true;
  SetEngineOptions(config);
  {
    Engine engine(data_directory);
    EXPECT_THROW(SetEngineOptions({}), std::runtime_error);
    if (const auto character = UsableCharacter(engine.catalog())) {
      EXPECT_GT(SynthesizedSamples(engine, *character), 0u);
    }
  }
  EXPECT_NO_THROW(SetEngineOptions({}));
}

}  // namespace
}  // namespace thespeon

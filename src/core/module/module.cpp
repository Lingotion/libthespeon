// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/module/module.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/utils/profile.h"

namespace thespeon {
namespace {

using nlohmann::json;

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
  return ToLower(left) == ToLower(right);
}

std::string JsonString(const json& value, const char* key) {
  const auto found = value.find(key);
  return found != value.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

json LoadConfig(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("Could not open module config: " + path.string());
  return json::parse(input);
}

void ParseFiles(const json& config,
                std::unordered_map<std::string, FileReference>& destination) {
  const auto files = config.find("files");
  if (files == config.end() || !files->is_array()) return;
  for (const auto& file : *files) {
    const auto name = JsonString(file, "name");
    const auto md5 = JsonString(file, "md5");
    const auto extension = JsonString(file, "extension");
    if (!name.empty() && !md5.empty() && !extension.empty())
      destination[name] = {md5, extension};
  }
}

void ParseStringIntMap(
    const json& object, const char* key,
    std::unordered_map<std::string, std::int64_t>& destination) {
  const auto values = object.find(key);
  if (values == object.end() || !values->is_object()) return;
  for (auto it = values->begin(); it != values->end(); ++it) {
    if (it.value().is_number_integer())
      destination[it.key()] = it.value().get<std::int64_t>();
  }
}

const FileReference& RequireFile(
    const std::unordered_map<std::string, FileReference>& files,
    const std::string& name, const std::string& description) {
  const auto found = files.find(name);
  if (found == files.end())
    throw std::runtime_error(description + " is missing its " + name + " file");
  return found->second;
}

Version ParseVersion(const json& config) {
  Version result;
  const auto version = config.find("version");
  if (version == config.end() || !version->is_object()) return result;
  const auto read = [&](const char* key, int& destination) {
    const auto found = version->find(key);
    if (found != version->end() && found->is_number_integer())
      destination = found->get<int>();
  };
  read("major", result.major);
  read("minor", result.minor);
  read("patch", result.patch);
  return result;
}

std::vector<Language> ParseLanguages(const json& config) {
  std::vector<Language> result;
  const auto list = config.find("languages");
  if (list == config.end() || !list->is_array()) return result;
  for (const auto& value : *list) {
    Language language;
    language.iso639_2 = ToLower(JsonString(value, "iso639_2"));
    language.country = JsonString(value, "iso3166_1");
    language.iso639_3 = JsonString(value, "iso639_3");
    language.glottocode = JsonString(value, "glottocode");
    language.iso3166_2 = JsonString(value, "iso3166_2");
    language.custom_dialect = JsonString(value, "customDialect");
    if (language.custom_dialect.empty())
      language.custom_dialect = JsonString(value, "customdialect");
    language.name_in_english = JsonString(value, "nameinenglish");
    language.autonym = JsonString(value, "autonym");
    result.push_back(std::move(language));
  }
  return result;
}

// Keeps only the entries EmotionKey accepts.
std::vector<EmotionDescription> ParseEmotions(const json& config) {
  std::vector<EmotionDescription> result;
  const auto list = config.find("emotionsets");
  if (list == config.end() || !list->is_array()) return result;
  for (const auto& value : *list) {
    auto name = JsonString(value, "emotionsetname");
    const auto key = value.find("emotionsetkey");
    if (name.empty() || key == value.end() || !key->is_number_integer() ||
        key->get<std::int64_t>() <= 0)
      continue;
    result.push_back({std::move(name), JsonString(value, "emotionsetguide")});
  }
  return result;
}

template <typename ModuleValue>
void ParseCommonModule(const json& config, const std::filesystem::path& path,
                       ModuleValue& module) {
  module.identifier = JsonString(config, "module_identifier");
  module.config_path = path;
  module.version = ParseVersion(config);
}

CharacterModule ParseCharacter(const json& config,
                               const std::filesystem::path& path) {
  CharacterModule module;
  ParseCommonModule(config, path, module);
  const auto& character = config.at("character");
  module.character_name = character.at("charactername").get<std::string>();
  if (module.character_name.empty())
    throw std::runtime_error("invalid character metadata");

  const auto tags = config.find("tags");
  module.quality =
      tags != config.end() ? JsonString(*tags, "module_type") : "unknown";
  module.languages = ParseLanguages(config);
  module.emotions = ParseEmotions(config);
  return module;
}

LanguageModule ParseLanguageModule(const json& config,
                                   const std::filesystem::path& path) {
  LanguageModule module;
  ParseCommonModule(config, path, module);
  module.name = JsonString(config, "name");
  module.languages = ParseLanguages(config);
  return module;
}

bool SupportsLanguageValue(const LanguageModule& module,
                           const std::string& language) {
  return std::any_of(module.languages.begin(), module.languages.end(),
                     [&](const Language& value) {
                       return EqualsIgnoreCase(value.iso639_2, language);
                     });
}

std::string RequiredLanguageModuleIdValue(const CharacterModule& character,
                                          const std::string& language) {
  const auto config = LoadConfig(character.config_path);
  const auto setup = config.find("phonemizer_setup");
  if (setup == config.end() || !setup->is_object()) return {};
  const auto modules = setup->find("modules");
  if (modules == setup->end() || !modules->is_array()) return {};
  for (const auto& entry : *modules) {
    if (EqualsIgnoreCase(JsonString(entry, "iso639_2"), language))
      return JsonString(entry, "base_module_id");
  }
  return {};
}

std::string BaseModuleIdValue(const LanguageModule& module) {
  return JsonString(LoadConfig(module.config_path), "base_module_id");
}

std::string SizeName(int rank) {
  switch (rank) {
    case 5: return "XL";
    case 4: return "L";
    case 3: return "M";
    case 2: return "S";
    case 1: return "XS";
    default: return {};
  }
}

// Module configs spell the five sizes their own way.
int QualityRank(const std::string& quality) {
  const auto value = ToLower(quality);
  if (value == "ultrahigh") return 5;
  if (value == "high") return 4;
  if (value == "mid") return 3;
  if (value == "low") return 2;
  if (value == "ultralow") return 1;
  return 0;
}

}  // namespace

CharacterModule LoadCharacterModule(const std::filesystem::path& path) {
  return ParseCharacter(LoadConfig(path), path);
}

LanguageModule LoadLanguageModule(const std::filesystem::path& path) {
  return ParseLanguageModule(LoadConfig(path), path);
}

std::optional<ModuleType> ReadModuleType(const std::filesystem::path& path) {
  const auto type = JsonString(LoadConfig(path), "type");
  if (type == "lara") return ModuleType::Character;
  if (type == "phonemizer") return ModuleType::Language;
  return std::nullopt;
}

bool SupportsLanguage(const LanguageModule& module,
                      const std::string& language) {
  return SupportsLanguageValue(module, language);
}

std::string RequiredLanguageModuleId(const CharacterModule& character,
                                     const std::string& language) {
  return RequiredLanguageModuleIdValue(character, language);
}

std::string BaseModuleId(const LanguageModule& module) {
  return BaseModuleIdValue(module);
}

const EmotionDescription* FindEmotion(const CharacterModule& character,
                                      const std::string& name) {
  for (const auto& emotion : character.emotions) {
    if (EqualsIgnoreCase(emotion.name, name)) return &emotion;
  }
  return nullptr;
}

int ModuleTypeRank(const std::string& module_type) {
  const auto value = ToLower(module_type);
  if (value == "xl") return 5;
  if (value == "l") return 4;
  if (value == "m") return 3;
  if (value == "s") return 2;
  if (value == "xs") return 1;
  return 0;
}

std::string ModuleTypeName(const std::string& quality) {
  const auto name = SizeName(QualityRank(quality));
  return name.empty() ? quality : name;
}

Version ParseVersionString(const std::string& value) {
  std::istringstream input(value.rfind("v", 0) == 0 ? value.substr(1) : value);
  Version result;
  char first = 0;
  char second = 0;
  input >> result.major >> first >> result.minor >> second >> result.patch;
  if (input.fail() || !input.eof() || first != '.' || second != '.') return {};
  return result.IsValid() ? result : Version{};
}

bool Version::IsValid() const { return major >= 0 && minor >= 0 && patch >= 0; }

std::string Version::ToString(bool prefix_v) const {
  std::ostringstream output;
  if (prefix_v) output << 'v';
  output << major << '.' << minor << '.' << patch;
  return output.str();
}

bool operator<(const Version& left, const Version& right) {
  return std::tie(left.major, left.minor, left.patch) <
         std::tie(right.major, right.minor, right.patch);
}

std::string Language::Display() const {
  return country.empty() ? iso639_2 : iso639_2 + "-" + country;
}

std::vector<std::pair<std::string, std::string>> Language::Fields() const {
  const std::pair<const char*, const std::string*> all[] = {
      {"iso639_2", &iso639_2},
      {"iso639_3", &iso639_3},
      {"glottocode", &glottocode},
      {"iso3166_1", &country},
      {"iso3166_2", &iso3166_2},
      {"customdialect", &custom_dialect},
      {"nameinenglish", &name_in_english},
      {"autonym", &autonym}};

  std::vector<std::pair<std::string, std::string>> result;
  for (const auto& [name, value] : all) {
    if (!value->empty()) result.emplace_back(name, *value);
  }
  return result;
}

std::string Language::DisplayFull() const {
  std::string result;
  for (const auto& [name, value] : Fields()) {
    if (!result.empty()) result += " ";
    result += name + "=" + value;
  }
  return result;
}

std::filesystem::path FileReference::Resolve(
    const std::filesystem::path& binaries) const {
  return binaries / (md5 + "." + extension);
}

std::filesystem::path CharacterConfig::MetagraphPath(
    const std::filesystem::path& binaries) const {
  return RequireFile(files, "metagraph", "Character module " + name)
      .Resolve(binaries);
}

std::int64_t CharacterConfig::LanguageKey(const std::string& iso639_2) const {
  for (const auto& language : languages) {
    if (EqualsIgnoreCase(language.iso639_2, iso639_2) && language.key > 0)
      return language.key;
  }
  throw std::runtime_error("Character module has no language key for " +
                           iso639_2);
}

std::int64_t CharacterConfig::EmotionKey(const std::string& emotion) const {
  for (const auto& value : emotion_ids) {
    if (EqualsIgnoreCase(value.first, emotion) && value.second > 0)
      return value.second;
  }
  throw std::runtime_error("Character module has no " + emotion +
                           " emotion key");
}

std::filesystem::path LanguageConfig::MetagraphPath(
    const std::filesystem::path& binaries) const {
  return RequireFile(files, "metagraph", "Language module " + name)
      .Resolve(binaries);
}

std::unordered_map<std::string, std::string> LanguageConfig::LoadLookupTable(
    const std::filesystem::path& binaries) const {
  THESPEON_PROFILE_SCOPE("LoadLookupTable");
  std::ifstream input(
      RequireFile(files, "lookuptable", "Language module " + name)
          .Resolve(binaries));
  if (!input) throw std::runtime_error("Could not open language lookup table");
  const auto table = json::parse(input);
  if (!table.is_object())
    throw std::runtime_error("Lookup table must be a JSON object");

  std::unordered_map<std::string, std::string> result;
  result.reserve(table.size());
  for (auto it = table.begin(); it != table.end(); ++it) {
    if (it.key() != "license_terms" && it.value().is_string())
      result.emplace(it.key(), it.value().get<std::string>());
  }
  return result;
}

CharacterConfig LoadCharacterConfig(const CharacterModule& module) {
  const auto config = LoadConfig(module.config_path);
  CharacterConfig result;
  result.name = module.character_name;
  const auto& character = config.at("character");
  result.character_key = character.at("characterkey").get<std::int64_t>();

  const auto languages = config.find("languages");
  if (languages != config.end() && languages->is_array()) {
    for (const auto& language : *languages) {
      RuntimeLanguage value;
      value.iso639_2 = ToLower(JsonString(language, "iso639_2"));
      const auto key = language.find("languagekey");
      if (key != language.end() && key->is_number_integer())
        value.key = key->get<std::int64_t>();
      result.languages.push_back(std::move(value));
    }
  }

  const auto table = config.find("phonemes_table");
  if (table != config.end())
    ParseStringIntMap(*table, "symbol_to_id", result.phoneme_ids);

  const auto emotions = config.find("emotionsets");
  if (emotions != config.end() && emotions->is_array()) {
    for (const auto& emotion : *emotions) {
      if (emotion.contains("emotionsetname") &&
          emotion.contains("emotionsetkey")) {
        result.emotion_ids[emotion.at("emotionsetname").get<std::string>()] =
            emotion.at("emotionsetkey").get<std::int64_t>();
      }
    }
  }
  ParseFiles(config, result.files);
  return result;
}

LanguageConfig LoadLanguageConfig(const LanguageModule& module) {
  const auto config = LoadConfig(module.config_path);
  LanguageConfig result;
  result.name = module.name;
  const auto vocabularies = config.find("vocabularies");
  if (vocabularies != config.end() && vocabularies->is_object()) {
    ParseStringIntMap(*vocabularies, "grapheme_vocab", result.grapheme_ids);
    const auto inverse = vocabularies->find("phoneme_ivocab");
    if (inverse != vocabularies->end() && inverse->is_object()) {
      for (auto it = inverse->begin(); it != inverse->end(); ++it) {
        if (!it.value().is_string()) continue;
        try {
          result.phoneme_symbols[std::stoll(it.key())] =
              it.value().get<std::string>();
        } catch (const std::exception&) {
        }
      }
    }
  }
  ParseFiles(config, result.files);
  return result;
}

}  // namespace thespeon

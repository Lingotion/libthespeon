// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace thespeon {

struct Version {
  int major = -1;
  int minor = -1;
  int patch = -1;

  bool IsValid() const;
  std::string ToString(bool prefix_v = true) const;

  friend bool operator==(const Version&, const Version&) = default;
  friend bool operator<(const Version& left, const Version& right);
  friend bool operator>(const Version& left, const Version& right) {
    return right < left;
  }
  friend bool operator<=(const Version& left, const Version& right) {
    return !(right < left);
  }
  friend bool operator>=(const Version& left, const Version& right) {
    return !(left < right);
  }
};

struct Language {
  std::string iso639_2;
  std::string country;
  std::string iso639_3;
  std::string glottocode;
  std::string iso3166_2;
  std::string custom_dialect;
  std::string name_in_english;
  std::string autonym;

  // "eng" or "eng-US", for naming one language in passing.
  std::string Display() const;
  // Every field the config actually set, keyed by its config name.
  std::vector<std::pair<std::string, std::string>> Fields() const;
  // Those fields as "key=value key=value", for listings.
  std::string DisplayFull() const;
};

// One emotion a character module accepts by name.
struct EmotionDescription {
  std::string name;
  std::string guide;
};

enum class ModuleType { Character, Language };

class Module {
 public:
  virtual ~Module() = default;
  virtual ModuleType module_type() const noexcept = 0;

  // Empty in packs built before the field existed.
  std::string identifier;
  std::filesystem::path config_path;
  Version version;
};

class CharacterModule final : public Module {
 public:
  ModuleType module_type() const noexcept override {
    return ModuleType::Character;
  }

  std::string character_name;
  std::string quality;
  std::vector<Language> languages;
  std::vector<EmotionDescription> emotions;
};

class LanguageModule final : public Module {
 public:
  ModuleType module_type() const noexcept override {
    return ModuleType::Language;
  }

  std::string name;
  std::vector<Language> languages;
};

// A file shipped alongside a module, addressed by content hash.
struct FileReference {
  std::string md5;
  std::string extension;

  std::filesystem::path Resolve(const std::filesystem::path& binaries) const;
};

// The language identifier a character module's model was trained with.
struct RuntimeLanguage {
  std::string iso639_2;
  std::int64_t key = 0;
};

// The contents of a character module's config file.
struct CharacterConfig {
  std::string name;
  std::int64_t character_key = 0;
  std::vector<RuntimeLanguage> languages;
  std::unordered_map<std::string, std::int64_t> phoneme_ids;
  std::unordered_map<std::string, std::int64_t> emotion_ids;
  std::unordered_map<std::string, FileReference> files;

  std::filesystem::path MetagraphPath(
      const std::filesystem::path& binaries) const;
  std::int64_t LanguageKey(const std::string& iso639_2) const;
  std::int64_t EmotionKey(const std::string& emotion) const;
};

// The contents of a language module's config file.
struct LanguageConfig {
  std::string name;
  std::unordered_map<std::string, std::int64_t> grapheme_ids;
  std::unordered_map<std::int64_t, std::string> phoneme_symbols;
  std::unordered_map<std::string, FileReference> files;

  std::filesystem::path MetagraphPath(
      const std::filesystem::path& binaries) const;
  std::unordered_map<std::string, std::string> LoadLookupTable(
      const std::filesystem::path& binaries) const;
  // Packs built before text preprocessing moved into them have none.
  std::filesystem::path TextPreprocessingPath(
      const std::filesystem::path& binaries) const;
};

CharacterModule LoadCharacterModule(const std::filesystem::path& path);
LanguageModule LoadLanguageModule(const std::filesystem::path& path);
std::optional<ModuleType> ReadModuleType(const std::filesystem::path& path);
CharacterConfig LoadCharacterConfig(const CharacterModule& module);
LanguageConfig LoadLanguageConfig(const LanguageModule& module);

bool SupportsLanguage(const LanguageModule& module,
                      const std::string& language);
std::string RequiredLanguageModuleId(const CharacterModule& character,
                                     const std::string& language);
std::string BaseModuleId(const LanguageModule& module);
// Matches case-insensitively; null when the character has no such emotion.
const EmotionDescription* FindEmotion(const CharacterModule& character,
                                      const std::string& name);

// "XS" through "XL", 0 for anything else.
int ModuleTypeRank(const std::string& module_type);
// A module's own quality spelling as one of those five.
std::string ModuleTypeName(const std::string& quality);

// Accepts an optional leading "v"; invalid on failure.
Version ParseVersionString(const std::string& value);

}  // namespace thespeon

// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/module/catalog.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

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

int QualityRank(const std::string& quality) {
  return ModuleTypeRank(ModuleTypeName(quality));
}

std::string SizeName(int rank) {
  static constexpr const char* names[] = {"", "XS", "S", "M", "L", "XL"};
  return rank >= 1 && rank <= 5 ? names[rank] : std::string{};
}

std::string InstalledSummary(
    const std::vector<const CharacterModule*>& matches) {
  std::string result;
  for (const auto* module : matches) {
    if (!result.empty()) result += ", ";
    result += ModuleTypeName(module->quality);
    if (module->version.IsValid()) result += " " + module->version.ToString();
  }
  return result;
}

void Narrow(std::vector<const CharacterModule*>& matches,
            const std::string& module_type, const Version& version,
            const std::string& subject) {
  const auto keep = [&](const auto& predicate, const std::string& wanted) {
    std::vector<const CharacterModule*> kept;
    for (const auto* module : matches) {
      if (predicate(*module)) kept.push_back(module);
    }
    if (kept.empty())
      throw std::runtime_error("No " + wanted + " character module is " +
                               "installed for " + subject + "; installed: " +
                               InstalledSummary(matches));
    matches.swap(kept);
  };

  if (!module_type.empty()) {
    const auto rank = ModuleTypeRank(module_type);
    keep([&](const CharacterModule& module) {
      return QualityRank(module.quality) == rank;
    }, SizeName(rank));
  }
  if (version.IsValid()) {
    keep([&](const CharacterModule& module) {
      return module.version == version;
    }, "version " + version.ToString(false));
  }
}

bool HasIndistinguishableBest(
    const std::vector<const CharacterModule*>& matches) {
  return matches.size() > 1 &&
         QualityRank(matches[0]->quality) ==
             QualityRank(matches[1]->quality) &&
         matches[0]->version == matches[1]->version;
}

void SortMatches(std::vector<const CharacterModule*>& matches) {
  std::sort(matches.begin(), matches.end(), [](const auto* left,
                                                const auto* right) {
    const auto left_rank = QualityRank(left->quality);
    const auto right_rank = QualityRank(right->quality);
    if (left_rank != right_rank) return left_rank > right_rank;
    if (left->version == right->version)
      return left->config_path < right->config_path;
    return right->version < left->version;
  });
}

}  // namespace

Catalog Catalog::Load(const std::filesystem::path& data_directory) {
  Catalog catalog;
  const auto configs = data_directory / "configs";
  if (!std::filesystem::exists(configs)) return catalog;

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(configs)) {
    if (entry.is_regular_file() &&
        ToLower(entry.path().extension().string()) == ".json")
      paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths) {
    try {
      const auto type = ReadModuleType(path);
      if (type == ModuleType::Character) {
        catalog.character_modules_.push_back(LoadCharacterModule(path));
      } else if (type == ModuleType::Language) {
        catalog.language_modules_.push_back(LoadLanguageModule(path));
      }
    } catch (const std::exception& error) {
      catalog.problems_.push_back(path.filename().string() + ": " +
                                  error.what());
    }
  }

  std::sort(catalog.character_modules_.begin(),
            catalog.character_modules_.end(),
            [](const auto& left, const auto& right) {
              return left.character_name < right.character_name;
            });
  std::sort(catalog.language_modules_.begin(), catalog.language_modules_.end(),
            [](const auto& left, const auto& right) {
              return left.identifier < right.identifier;
            });
  return catalog;
}

Catalog Catalog::Without(const std::filesystem::path& config_path) const {
  Catalog result;
  result.problems_ = problems_;
  for (const auto& module : character_modules_) {
    if (module.config_path != config_path)
      result.character_modules_.push_back(module);
  }
  for (const auto& module : language_modules_) {
    if (module.config_path != config_path)
      result.language_modules_.push_back(module);
  }
  return result;
}

const CharacterModule& Catalog::SelectCharacter(
    const std::string& selector, const std::string& module_type,
    const Version& version) const {
  std::vector<const CharacterModule*> matches;
  for (const auto& module : character_modules_) {
    if (EqualsIgnoreCase(module.character_name, selector))
      matches.push_back(&module);
  }
  if (matches.empty())
    throw std::runtime_error("No character module named \"" + selector +
                             "\" is installed");

  Narrow(matches, module_type, version, "\"" + selector + "\"");
  SortMatches(matches);
  if (HasIndistinguishableBest(matches)) {
    throw std::runtime_error(
        "More than one character module named \"" + selector +
        "\" is installed; name it by module identifier " +
        matches[0]->identifier + " or " + matches[1]->identifier);
  }
  return *matches.front();
}

const CharacterModule* Catalog::FindCharacterByIdentifier(
    const std::string& identifier) const {
  for (const auto& module : character_modules_) {
    if (EqualsIgnoreCase(module.identifier, identifier)) return &module;
  }
  return nullptr;
}

const LanguageModule* Catalog::FindLanguageByIdentifier(
    const std::string& identifier) const {
  for (const auto& module : language_modules_) {
    if (EqualsIgnoreCase(module.identifier, identifier)) return &module;
  }
  return nullptr;
}

const LanguageModule& Catalog::CompatibleLanguageModule(
    const CharacterModule& character, std::string& language,
    const std::string& requested) const {
  constexpr auto kDefaultLanguage = "eng";
  const std::string wanted = requested.empty() ? kDefaultLanguage : requested;
  const auto required = RequiredLanguageModuleId(character, wanted);
  if (!required.empty()) {
    for (const auto& module : language_modules_) {
      if (SupportsLanguage(module, wanted) &&
          BaseModuleId(module) == required) {
        language = wanted;
        return module;
      }
    }
  }
  for (const auto& module : language_modules_) {
    if (SupportsLanguage(module, wanted)) {
      language = wanted;
      return module;
    }
  }
  throw std::runtime_error("No compatible language module for \"" + wanted +
                           "\" is installed for \"" +
                           character.character_name + "\"");
}

namespace {

json EmotionArray(const CharacterModule& module) {
  json result = json::array();
  for (const auto& emotion : module.emotions)
    result.push_back({{"name", emotion.name}, {"guide", emotion.guide}});
  return result;
}

}  // namespace

std::string CatalogJson(const Catalog& catalog) {
  json result = {{"characterModules", json::array()},
                 {"languageModules", json::array()},
                 {"problems", catalog.problems()}};

  for (const auto& module : catalog.character_modules()) {
    json languages = json::array();
    for (const auto& language : module.languages) {
      languages.push_back({{"iso639_2", language.iso639_2},
                           {"country", language.country}});
    }
    result["characterModules"].push_back(
        {{"identifier", module.identifier},
         {"name", module.character_name},
         {"moduleType", ModuleTypeName(module.quality)},
         {"version", module.version.ToString(false)},
         {"languages", std::move(languages)},
         {"emotions", EmotionArray(module)}});
  }

  for (const auto& module : catalog.language_modules()) {
    json languages = json::array();
    for (const auto& language : module.languages) {
      json fields = json::object();
      for (const auto& [name, value] : language.Fields()) fields[name] = value;
      languages.push_back(std::move(fields));
    }
    result["languageModules"].push_back(
        {{"identifier", module.identifier},
         {"version", module.version.ToString(false)},
         {"languages", std::move(languages)}});
  }
  return result.dump();
}

std::string EmotionsJson(const CharacterModule& module) {
  return EmotionArray(module).dump();
}

Version ParseNarrowing(const std::string& module_type,
                       const std::string& module_version) {
  if (!module_type.empty() && ModuleTypeRank(module_type) == 0)
    throw std::runtime_error("Module type expects one of XS, S, M, L, XL");
  const auto version = ParseVersionString(module_version);
  if (!module_version.empty() && !version.IsValid())
    throw std::runtime_error("Module version expects a version such as 3.0.1");
  return version;
}

}  // namespace thespeon

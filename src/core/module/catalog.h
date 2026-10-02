// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/module/module.h"

namespace thespeon {

class Catalog {
 public:
  static Catalog Load(const std::filesystem::path& data_directory);

  // A copy without the module that config_path names, for answering what the
  // catalog would look like once it is deleted.
  Catalog Without(const std::filesystem::path& config_path) const;

  const std::vector<CharacterModule>& character_modules() const {
    return character_modules_;
  }
  const std::vector<LanguageModule>& language_modules() const {
    return language_modules_;
  }
  const std::vector<std::string>& problems() const { return problems_; }

  // The selector is a module identifier or a character name. For a name,
  // unset narrowing takes the largest size and its newest version.
  const CharacterModule& SelectCharacter(const std::string& selector,
                                         const std::string& module_type = {},
                                         const Version& version = {}) const;
  // Null when nothing matches.
  const CharacterModule* FindCharacterByIdentifier(
      const std::string& identifier) const;
  const LanguageModule* FindLanguageByIdentifier(
      const std::string& identifier) const;
  // An empty requested language means English.
  const LanguageModule& CompatibleLanguageModule(
      const CharacterModule& character, std::string& language,
      const std::string& requested = {}) const;

 private:
  std::vector<CharacterModule> character_modules_;
  std::vector<LanguageModule> language_modules_;
  std::vector<std::string> problems_;
};

// The installed modules as a JSON document, for hosts that list them.
std::string CatalogJson(const Catalog& catalog);
// One character's emotions as the array CatalogJson lists under it.
std::string EmotionsJson(const CharacterModule& module);

// Either value may be empty; throws on a bad size or version.
Version ParseNarrowing(const std::string& module_type,
                       const std::string& module_version);

}  // namespace thespeon

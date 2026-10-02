// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/module/module.h"

namespace thespeon {

struct ModuleLanguage {
  std::string iso639_2;
  std::optional<std::string> iso639_3;
  std::optional<std::string> glottocode;
  std::optional<std::string> iso3166_1;
  std::optional<std::string> iso3166_2;
  std::optional<std::string> custom_dialect;
  std::optional<std::string> legacy_custom_dialect;
};

using EmotionWeights = std::unordered_map<std::string, double>;

struct SynthInputSegmentV100 {
  std::string text;
  std::optional<std::string> emotion;
  std::optional<EmotionWeights> start_emotion;
  std::optional<EmotionWeights> end_emotion;
  std::optional<ModuleLanguage> language;
  bool is_custom_pronounced = false;
  std::optional<double> start_speed;
  std::optional<double> end_speed;
  std::optional<double> start_loudness;
  std::optional<double> end_loudness;
};

struct SynthInputV100 {
  std::string version = "1.0.0";
  std::vector<SynthInputSegmentV100> segments;
  std::string module_identifier;
  std::string character_name;
  std::optional<std::string> module_type;
  Version module_version;
  // Document keys left unread beside an identifier.
  std::vector<std::string> ignored_fields;
  std::optional<std::string> default_emotion;
  ModuleLanguage default_language;
};

using ThespeonInput = std::variant<SynthInputV100>;

ThespeonInput ParseThespeonInput(std::string_view json);
std::string ConcatenateSegmentText(const ThespeonInput& input);
// The lowercased iso639_2 code of each segment: its own language, else the
// document default, else English.
std::vector<std::string> SegmentLanguages(const ThespeonInput& input);
struct CharacterSelection {
  std::string module_identifier;
  std::string character_name;
  std::string module_type;
  Version module_version;
  std::vector<std::string> ignored_fields;
};

CharacterSelection SelectedCharacter(const ThespeonInput& input);

}  // namespace thespeon

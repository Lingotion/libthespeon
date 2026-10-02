// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/input/thespeon_input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "core/module/module.h"

namespace thespeon {
namespace {

using nlohmann::json;

[[noreturn]] void Invalid(const std::string& path,
                          const std::string& problem) {
  throw std::invalid_argument(path + ": " + problem);
}

std::string Trim(std::string value) {
  const auto not_space = [](unsigned char character) {
    return !std::isspace(character);
  };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

const json& RequireObjectMember(const json& object, const char* key,
                                const std::string& path) {
  const auto found = object.find(key);
  if (found == object.end()) Invalid(path + "." + key, "Required");
  if (!found->is_object()) Invalid(path + "." + key, "Expected object");
  return *found;
}

std::string RequireString(const json& object, const char* key,
                          const std::string& path) {
  const auto found = object.find(key);
  if (found == object.end()) Invalid(path + "." + key, "Required");
  if (!found->is_string()) Invalid(path + "." + key, "Expected string");
  return found->get<std::string>();
}

std::optional<std::string> OptionalString(const json& object, const char* key,
                                          const std::string& path) {
  const auto found = object.find(key);
  if (found == object.end()) return std::nullopt;
  if (!found->is_string()) Invalid(path + "." + key, "Expected string");
  return found->get<std::string>();
}

std::string EmotionReference(const std::string& value,
                             const std::string& path) {
  auto result = Trim(value);
  if (result.empty()) Invalid(path, "Required");
  return result;
}

std::optional<std::string> OptionalEmotionReference(
    const json& object, const char* key, const std::string& path) {
  auto result = OptionalString(object, key, path);
  if (result) *result = EmotionReference(*result, path + "." + key);
  return result;
}

std::optional<double> OptionalPositiveNumber(const json& object,
                                             const char* key,
                                             const std::string& path) {
  const auto found = object.find(key);
  if (found == object.end()) return std::nullopt;
  if (!found->is_number()) Invalid(path + "." + key, "Expected number");
  const auto value = found->get<double>();
  if (!std::isfinite(value) || value <= 0.0)
    Invalid(path + "." + key, "Expected positive number");
  return value;
}

ModuleLanguage ParseLanguage(const json& value, const std::string& path) {
  if (!value.is_object()) Invalid(path, "Expected object");
  ModuleLanguage result;
  result.iso639_2 = RequireString(value, "iso639_2", path);
  result.iso639_3 = OptionalString(value, "iso639_3", path);
  result.glottocode = OptionalString(value, "glottocode", path);
  result.iso3166_1 = OptionalString(value, "iso3166_1", path);
  result.iso3166_2 = OptionalString(value, "iso3166_2", path);
  result.custom_dialect = OptionalString(value, "customDialect", path);
  result.legacy_custom_dialect =
      OptionalString(value, "customdialect", path);
  return result;
}

std::optional<ModuleLanguage> OptionalLanguage(const json& object,
                                               const char* key,
                                               const std::string& path) {
  const auto found = object.find(key);
  if (found == object.end()) return std::nullopt;
  return ParseLanguage(*found, path + "." + key);
}

EmotionWeights ParseEmotionWeights(const json& value,
                                   const std::string& path) {
  if (!value.is_object()) Invalid(path, "Expected object");
  EmotionWeights result;
  for (auto it = value.begin(); it != value.end(); ++it) {
    const auto name = EmotionReference(it.key(), path);
    if (!it.value().is_number())
      Invalid(path + "." + it.key(), "Expected number");
    const auto weight = it.value().get<double>();
    if (!std::isfinite(weight) || weight < 0.0)
      Invalid(path + "." + it.key(), "Expected nonnegative number");
    result[name] = weight;
  }
  return result;
}

std::optional<EmotionWeights> OptionalEmotionWeights(
    const json& object, const char* key, const std::string& path) {
  const auto found = object.find(key);
  if (found == object.end()) return std::nullopt;
  return ParseEmotionWeights(*found, path + "." + key);
}

SynthInputSegmentV100 ParseSegment(const json& value, std::size_t index) {
  const auto path = "$.segments[" + std::to_string(index) + "]";
  if (!value.is_object()) Invalid(path, "Expected object");

  SynthInputSegmentV100 result;
  result.text = RequireString(value, "text", path);
  if (Trim(result.text).empty()) Invalid(path + ".text", "Required");
  result.emotion = OptionalEmotionReference(value, "emotion", path);
  result.start_emotion =
      OptionalEmotionWeights(value, "startEmotion", path);
  result.end_emotion = OptionalEmotionWeights(value, "endEmotion", path);
  // The legacy single emotion pins whichever boundary was not given.
  if (result.emotion) {
    const EmotionWeights legacy{{*result.emotion, 1.0}};
    if (!result.start_emotion) result.start_emotion = legacy;
    if (!result.end_emotion) result.end_emotion = legacy;
  }
  result.language = OptionalLanguage(value, "language", path);

  const auto custom = value.find("isCustomPronounced");
  if (custom != value.end()) {
    if (!custom->is_boolean())
      Invalid(path + ".isCustomPronounced", "Expected boolean");
    result.is_custom_pronounced = custom->get<bool>();
  }

  result.start_speed = OptionalPositiveNumber(value, "startSpeed", path);
  result.end_speed = OptionalPositiveNumber(value, "endSpeed", path);
  result.start_loudness =
      OptionalPositiveNumber(value, "startLoudness", path);
  result.end_loudness =
      OptionalPositiveNumber(value, "endLoudness", path);
  return result;
}

SynthInputV100 ParseV100(const json& root) {
  SynthInputV100 result;
  result.version = RequireString(root, "version", "$");
  if (result.version != "1.0.0")
    Invalid("$.version", "Expected literal 1.0.0");

  const auto segments = root.find("segments");
  if (segments == root.end()) Invalid("$.segments", "Required");
  if (!segments->is_array()) Invalid("$.segments", "Expected array");
  if (segments->empty()) Invalid("$.segments", "Expected at least one segment");
  result.segments.reserve(segments->size());
  for (std::size_t index = 0; index < segments->size(); ++index)
    result.segments.push_back(ParseSegment((*segments)[index], index));

  // An identifier names one module, so the rest goes unread, not validated.
  if (const auto identifier = OptionalString(root, "moduleIdentifier", "$")) {
    result.module_identifier = Trim(*identifier);
    if (result.module_identifier.empty())
      Invalid("$.moduleIdentifier", "Required");
    for (const char* key : {"characterName", "moduleType", "moduleVersion"}) {
      if (root.contains(key)) result.ignored_fields.push_back(key);
    }
  } else {
    const auto name = OptionalString(root, "characterName", "$");
    if (!name) Invalid("$", "Expected moduleIdentifier or characterName");
    result.character_name = Trim(*name);
    if (result.character_name.empty()) Invalid("$.characterName", "Required");

    result.module_type = OptionalString(root, "moduleType", "$");
    if (result.module_type && ModuleTypeRank(*result.module_type) == 0)
      Invalid("$.moduleType", "Expected one of XS, S, M, L, XL");
    if (const auto version = OptionalString(root, "moduleVersion", "$")) {
      result.module_version = ParseVersionString(*version);
      if (!result.module_version.IsValid())
        Invalid("$.moduleVersion", "Expected a version such as 3.0.1");
    }
  }

  result.default_emotion =
      OptionalEmotionReference(root, "defaultEmotion", "$");
  result.default_language = ParseLanguage(
      RequireObjectMember(root, "defaultLanguage", "$"), "$.defaultLanguage");
  return result;
}

}  // namespace

ThespeonInput ParseThespeonInput(std::string_view input) {
  try {
    const auto root = json::parse(input.begin(), input.end());
    if (!root.is_object()) Invalid("$", "Expected object");
    const auto version = root.find("version");
    if (version == root.end()) Invalid("$.version", "Required");
    if (!version->is_string()) Invalid("$.version", "Expected string");
    if (version->get<std::string>() == "1.0.0") return ParseV100(root);
    Invalid("$.version", "Unsupported version");
  } catch (const nlohmann::json::exception& error) {
    throw std::invalid_argument(std::string("Invalid synthesis JSON: ") +
                                error.what());
  }
}

std::string ConcatenateSegmentText(const ThespeonInput& input) {
  const auto& value = std::get<SynthInputV100>(input);
  std::string result;
  for (const auto& segment : value.segments) result += segment.text;
  return result;
}

std::vector<std::string> SegmentLanguages(const ThespeonInput& input) {
  const auto& value = std::get<SynthInputV100>(input);
  const auto lower = [](std::string code) {
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    return code;
  };
  const auto fallback = value.default_language.iso639_2.empty()
                            ? std::string("eng")
                            : lower(value.default_language.iso639_2);
  std::vector<std::string> result;
  result.reserve(value.segments.size());
  for (const auto& segment : value.segments) {
    result.push_back(segment.language && !segment.language->iso639_2.empty()
                         ? lower(segment.language->iso639_2)
                         : fallback);
  }
  return result;
}

CharacterSelection SelectedCharacter(const ThespeonInput& input) {
  const auto& value = std::get<SynthInputV100>(input);
  return {value.module_identifier, value.character_name,
          value.module_type.value_or(std::string{}), value.module_version,
          value.ignored_fields};
}

}  // namespace thespeon

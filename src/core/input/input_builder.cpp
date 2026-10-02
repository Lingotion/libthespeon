// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/input/input_builder.h"

#include <cmath>
#include <stdexcept>

namespace thespeon {
namespace {

using nlohmann::json;

json LanguageJson(const ModuleLanguage& language) {
  json result = {{"iso639_2", language.iso639_2}};
  const auto put = [&](const char* key,
                       const std::optional<std::string>& value) {
    if (value) result[key] = *value;
  };
  put("iso639_3", language.iso639_3);
  put("glottocode", language.glottocode);
  put("iso3166_1", language.iso3166_1);
  put("iso3166_2", language.iso3166_2);
  put("customDialect", language.custom_dialect);
  return result;
}

void SetOrErase(json& object, const char* key,
                const std::optional<std::string>& value) {
  if (value)
    object[key] = *value;
  else
    object.erase(key);
}

// NaN and infinity would serialize as null.
double Finite(double value, const char* name) {
  if (!std::isfinite(value))
    throw std::invalid_argument(std::string(name) + " is not finite");
  return value;
}

const char* Key(Endpoint at, const char* start, const char* end) {
  return at == Endpoint::Start ? start : end;
}

}  // namespace

InputBuilder::InputBuilder()
    : document_{{"version", "1.0.0"},
                {"defaultLanguage", {{"iso639_2", "eng"}}},
                {"segments", json::array()}} {}

InputBuilder InputBuilder::FromJson(std::string_view json) {
  ParseThespeonInput(json);
  InputBuilder result;
  result.document_ = json::parse(json.begin(), json.end());
  return result;
}

void InputBuilder::SetCharacterModule(const std::string& identifier) {
  for (const char* key : {"characterName", "moduleType", "moduleVersion"})
    document_.erase(key);
  document_["moduleIdentifier"] = identifier;
}

void InputBuilder::SetCharacter(
    const std::string& name, const std::optional<std::string>& module_type,
    const std::optional<std::string>& module_version) {
  document_.erase("moduleIdentifier");
  document_["characterName"] = name;
  SetOrErase(document_, "moduleType", module_type);
  SetOrErase(document_, "moduleVersion", module_version);
}

void InputBuilder::SetDefaultEmotion(
    const std::optional<std::string>& emotion) {
  SetOrErase(document_, "defaultEmotion", emotion);
}

void InputBuilder::SetDefaultLanguage(const ModuleLanguage& language) {
  document_["defaultLanguage"] = LanguageJson(language);
}

std::size_t InputBuilder::AddSegment(const std::string& text) {
  auto& segments = document_["segments"];
  segments.push_back({{"text", text}});
  return segments.size() - 1;
}

void InputBuilder::SetCustomPronounced(std::size_t segment, bool enabled) {
  Segment(segment)["isCustomPronounced"] = enabled;
}

void InputBuilder::SetSegmentLanguage(
    std::size_t segment, const std::optional<ModuleLanguage>& language) {
  auto& value = Segment(segment);
  if (language)
    value["language"] = LanguageJson(*language);
  else
    value.erase("language");
}

void InputBuilder::SetSegmentEmotion(
    std::size_t segment, const std::optional<std::string>& emotion) {
  SetOrErase(Segment(segment), "emotion", emotion);
}

void InputBuilder::AddSegmentEmotion(std::size_t segment, Endpoint at,
                                     const std::string& emotion,
                                     double weight) {
  Segment(segment)[Key(at, "startEmotion", "endEmotion")][emotion] =
      Finite(weight, "weight");
}

void InputBuilder::SetSegmentSpeed(std::size_t segment, Endpoint at,
                                   double speed) {
  Segment(segment)[Key(at, "startSpeed", "endSpeed")] = Finite(speed, "speed");
}

void InputBuilder::SetSegmentLoudness(std::size_t segment, Endpoint at,
                                      double loudness) {
  Segment(segment)[Key(at, "startLoudness", "endLoudness")] =
      Finite(loudness, "loudness");
}

ThespeonInput InputBuilder::Build() const {
  return ParseThespeonInput(document_.dump());
}

std::string InputBuilder::ToJson() const {
  auto serialized = document_.dump();
  ParseThespeonInput(serialized);
  return serialized;
}

json& InputBuilder::Segment(std::size_t index) {
  auto& segments = document_["segments"];
  if (index >= segments.size())
    throw std::invalid_argument("segment " + std::to_string(index) +
                                " is out of range");
  return segments[index];
}

}  // namespace thespeon

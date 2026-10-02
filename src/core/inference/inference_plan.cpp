// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/inference_plan.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

#include "core/inference/prosody_preprocess.h"
#include "core/inference/build_inputs.h"
#include "core/inference/text_preprocess.h"

namespace thespeon {

namespace {

// The phonemizer for one language: its config, lookup table, and the words
// that table is missing.
struct Phonemizer {
  LanguageConfig config;
  std::unordered_map<std::string, std::string> lookup;
  std::vector<std::string> unknown_words;
};

void CheckEmotion(const CharacterModule& character, const std::string& name,
                  const std::string& path) {
  auto folded = name;
  std::transform(folded.begin(), folded.end(), folded.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  if (folded == "none" || FindEmotion(character, name)) return;
  throw std::invalid_argument(path + ": Character \"" +
                              character.character_name +
                              "\" has no emotion \"" + name + "\"");
}

void CheckEmotions(const CharacterModule& character,
                   const SynthInputV100& input) {
  if (input.default_emotion)
    CheckEmotion(character, *input.default_emotion, "$.defaultEmotion");
  for (std::size_t index = 0; index < input.segments.size(); ++index) {
    const auto& segment = input.segments[index];
    const auto path = "$.segments[" + std::to_string(index) + "].";
    if (segment.emotion)
      CheckEmotion(character, *segment.emotion, path + "emotion");
    if (segment.start_emotion) {
      for (const auto& [name, weight] : *segment.start_emotion)
        CheckEmotion(character, name, path + "startEmotion");
    }
    if (segment.end_emotion) {
      for (const auto& [name, weight] : *segment.end_emotion)
        CheckEmotion(character, name, path + "endEmotion");
    }
  }
}

// Normalizes each text in place and returns its markers.
std::vector<std::vector<std::size_t>> NormalizeSegments(
    std::vector<SynthInputSegmentV100>& segments) {
  const auto count = segments.size();
  std::vector<std::vector<std::size_t>> markers(count);
  for (std::size_t index = 0; index < count; ++index) {
    auto& segment = segments[index];
    auto normalized = inference_detail::NormalizeSegment(
        segment.text, index > 0, index + 1 < count,
        segment.is_custom_pronounced, index);
    segment.text = std::move(normalized.text);
    markers[index] = std::move(normalized.markers);
    if (segment.text.empty())
      throw std::invalid_argument(
          "$.segments[" + std::to_string(index) +
          "].text: Empty after normalization");
  }
  return markers;
}

}  // namespace

void ValidatePlanInput(const CharacterModule& character,
                       const ThespeonInput& input) {
  auto value = std::get<SynthInputV100>(input);
  CheckEmotions(character, value);
  NormalizeSegments(value.segments);
}

struct InferencePlan::Impl {
  Impl(const CharacterModule& character,
       const std::unordered_map<std::string, LanguageModule>& language_modules,
       std::filesystem::path binaries_value, ThespeonInput input_value)
      : character_config(LoadCharacterConfig(character)),
        binaries(std::move(binaries_value)),
        input(std::move(input_value)),
        languages(SegmentLanguages(input)) {
    auto& value = std::get<SynthInputV100>(input);
    const auto count = value.segments.size();
    CheckEmotions(character, value);
    markers = NormalizeSegments(value.segments);
    PopulateEmotionKeypoints(value.segments, value.default_emotion);
    PopulateSpeedKeypoints(value.segments);
    PopulateLoudnessKeypoints(value.segments);

    std::unordered_map<std::string, std::size_t> phonemizer_of;
    std::vector<std::vector<SynthInputSegmentV100>> phonemizer_segments;
    segment_phonemizer.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto& language = languages[index];
      const auto [found, added] =
          phonemizer_of.emplace(language, phonemizers.size());
      if (added) {
        const auto module = language_modules.find(language);
        if (module == language_modules.end())
          throw std::runtime_error("No language module was resolved for \"" +
                                   language + "\"");
        phonemizers.push_back({LoadLanguageConfig(module->second), {}, {}});
        phonemizer_segments.emplace_back();
      }
      segment_phonemizer[index] = found->second;
      phonemizer_segments[found->second].push_back(value.segments[index]);
    }

    for (std::size_t index = 0; index < phonemizers.size(); ++index) {
      const auto& segments = phonemizer_segments[index];
      if (std::none_of(segments.begin(), segments.end(),
                       [](const auto& segment) {
                         return !segment.is_custom_pronounced;
                       }))
        continue;
      auto& phonemizer = phonemizers[index];
      phonemizer.lookup = phonemizer.config.LoadLookupTable(binaries);
      phonemizer.unknown_words =
          inference_detail::UnknownWords(segments, phonemizer.lookup);
    }
  }

  const std::vector<SynthInputSegmentV100>& segments() const {
    return std::get<SynthInputV100>(input).segments;
  }

  CharacterConfig character_config;
  std::filesystem::path binaries;
  ThespeonInput input;
  std::vector<std::string> languages;
  std::vector<std::vector<std::size_t>> markers;
  std::vector<Phonemizer> phonemizers;
  std::vector<std::size_t> segment_phonemizer;
};

InferencePlan::InferencePlan(
    const CharacterModule& character,
    const std::unordered_map<std::string, LanguageModule>& language_modules,
    std::filesystem::path binaries, ThespeonInput input)
    : impl_(std::make_unique<Impl>(character, language_modules,
                                   std::move(binaries), std::move(input))) {}

InferencePlan::~InferencePlan() = default;
InferencePlan::InferencePlan(InferencePlan&&) noexcept = default;
InferencePlan& InferencePlan::operator=(InferencePlan&&) noexcept = default;

std::vector<InferenceRequest> InferencePlan::PrepareLanguageModules() const {
  std::vector<InferenceRequest> result;
  for (const auto& phonemizer : impl_->phonemizers) {
    if (phonemizer.unknown_words.empty()) continue;
    result.push_back(InferenceRequest{
        phonemizer.config.MetagraphPath(impl_->binaries),
        inference_detail::BuildLanguageInputs(phonemizer.unknown_words,
                                              phonemizer.config)});
  }
  return result;
}

void InferencePlan::ApplyLanguageModuleResults(
    const std::vector<metagraph::Tensor>& results) {
  const auto expected = std::count_if(
      impl_->phonemizers.begin(), impl_->phonemizers.end(),
      [](const auto& phonemizer) { return !phonemizer.unknown_words.empty(); });
  if (results.size() != static_cast<std::size_t>(expected))
    throw std::invalid_argument(
        "Expected one result per language-module request");
  std::size_t next = 0;
  for (auto& phonemizer : impl_->phonemizers) {
    if (phonemizer.unknown_words.empty()) continue;
    inference_detail::AddLanguageResults(phonemizer.unknown_words,
                                         results[next++],
                                         phonemizer.config, phonemizer.lookup);
  }
}

InferenceRequest InferencePlan::PrepareCharacterModule(
    std::vector<std::string>* warnings) const {
  const auto& segments = impl_->segments();
  std::vector<std::vector<std::int64_t>> encoded;
  std::vector<std::vector<std::int64_t>> marker_tokens(segments.size());
  encoded.reserve(segments.size());
  for (std::size_t index = 0; index < segments.size(); ++index) {
    const auto& phonemizer =
        impl_->phonemizers[impl_->segment_phonemizer[index]];
    encoded.push_back(inference_detail::EncodeForCharacter(
        segments[index].text, impl_->markers[index], phonemizer.lookup,
        impl_->character_config, segments[index].is_custom_pronounced,
        &marker_tokens[index], warnings));
  }
  return InferenceRequest{
      impl_->character_config.MetagraphPath(impl_->binaries),
      inference_detail::BuildCharacterInputs(
          segments, encoded, marker_tokens, impl_->character_config,
          impl_->languages)};
}

}  // namespace thespeon

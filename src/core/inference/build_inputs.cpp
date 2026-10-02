// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/build_inputs.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "core/inference/control_characters.h"
#include "core/inference/text_preprocess.h"

namespace thespeon::inference_detail {
namespace {

std::vector<std::string> SortedNames(const EmotionWeights& blend) {
  std::vector<std::string> result;
  result.reserve(blend.size());
  for (const auto& entry : blend) result.push_back(entry.first);
  std::sort(result.begin(), result.end());
  return result;
}

double Lerp(double start, double end, double alpha) {
  return start + (end - start) * alpha;
}

}  // namespace

metagraph::TensorMap BuildLanguageInputs(
    const std::vector<std::string>& words, const LanguageConfig& config) {
  if (words.empty())
    throw std::runtime_error("No words require language-module processing");
  const auto id = [&](const std::string& token) {
    const auto found = config.grapheme_ids.find(token);
    if (found == config.grapheme_ids.end())
      throw std::runtime_error("Language-module vocabulary is missing " +
                               token);
    return found->second;
  };
  const auto start = id("<sos>");
  const auto end = id("<eos>");
  const auto padding = id("<pad>");
  const auto unknown = config.grapheme_ids.find("<unk>");

  std::vector<std::vector<std::int64_t>> rows;
  std::size_t width = 0;
  for (const auto& word : words) {
    std::vector<std::int64_t> row{start};
    for (const auto code_point : DecodeUtf8(word)) {
      const auto found = config.grapheme_ids.find(EncodeUtf8(code_point));
      if (found != config.grapheme_ids.end()) {
        row.push_back(found->second);
      } else if (unknown != config.grapheme_ids.end()) {
        row.push_back(unknown->second);
      } else {
        throw std::runtime_error("Language module cannot encode word \"" +
                                 word + "\"");
      }
    }
    row.push_back(end);
    width = std::max(width, row.size());
    rows.push_back(std::move(row));
  }

  std::vector<std::int64_t> source(words.size() * width, padding);
  for (std::size_t row = 0; row < rows.size(); ++row) {
    std::copy(rows[row].begin(), rows[row].end(),
              source.begin() + row * width);
  }
  const auto batch = static_cast<std::int64_t>(words.size());
  metagraph::TensorMap inputs;
  inputs["src"] = metagraph::Tensor::Int64(
      {batch, static_cast<std::int64_t>(width)}, std::move(source));
  return inputs;
}

void AddLanguageResults(
    const std::vector<std::string>& words, const metagraph::Tensor& result,
    const LanguageConfig& config,
    std::unordered_map<std::string, std::string>& lookup) {
  if (result.dtype() != metagraph::DType::Int64 ||
      result.shape().size() != 2 ||
      result.shape()[0] != static_cast<std::int64_t>(words.size()))
    throw std::runtime_error("Language module returned an unexpected tensor");

  const auto width = result.shape()[1];
  for (std::size_t row = 0; row < words.size(); ++row) {
    std::string phonemes;
    for (std::int64_t column = 1; column < width; ++column) {
      const auto token = result.int64_data()[
          row * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(column)];
      const auto symbol = config.phoneme_symbols.find(token);
      if (symbol == config.phoneme_symbols.end()) continue;
      if (symbol->second == "<eos>" || symbol->second == "<pad>") break;
      if (!symbol->second.empty() && symbol->second.front() != '<')
        phonemes += symbol->second;
    }
    if (phonemes.empty())
      throw std::runtime_error(
          "Language module produced no pronunciation for \"" + words[row] +
          "\"");
    lookup[words[row]] = std::move(phonemes);
  }
}

metagraph::TensorMap BuildCharacterInputs(
    const std::vector<SynthInputSegmentV100>& segments,
    const std::vector<std::vector<std::int64_t>>& encoded,
    const std::vector<std::vector<std::int64_t>>& marker_tokens,
    const CharacterConfig& config, const std::vector<std::string>& languages) {
  const auto marker = [&](char32_t code_point) {
    const auto found = config.phoneme_ids.find(EncodeUtf8(code_point));
    if (found == config.phoneme_ids.end())
      throw std::runtime_error(
          "Character module phoneme table is missing a sequence marker");
    return found->second;
  };

  std::vector<std::int64_t> phonemes{marker(control::kSequenceStart)};
  for (const auto& body : encoded)
    phonemes.insert(phonemes.end(), body.begin(), body.end());
  phonemes.push_back(marker(control::kSequenceEnd));
  if (phonemes.size() > 512)
    throw std::runtime_error(
        "Text exceeds the character module's 512-token limit; shorten the "
        "input");
  if (phonemes.size() <= 2)
    throw std::runtime_error("Text contains no speakable symbols");

  std::vector<std::string> distinct;
  std::unordered_map<std::string, std::size_t> emotion_row;
  for (const auto& segment : segments) {
    for (const auto* blend : {&segment.start_emotion, &segment.end_emotion}) {
      for (auto& name : SortedNames(**blend)) {
        if (emotion_row.emplace(name, distinct.size()).second)
          distinct.push_back(std::move(name));
      }
    }
  }

  const auto width = phonemes.size();
  const auto rows = distinct.size();
  std::vector<float> blending(rows * width, 0.0f);
  std::vector<float> speed(2 * width, 1.0f);
  std::vector<float> loudness(width, 1.0f);
  std::vector<std::int64_t> language_keys(width);

  const auto to_vector = [&](const EmotionWeights& blend) {
    std::vector<float> result(rows, 0.0f);
    for (const auto& [name, weight] : blend)
      result[emotion_row.at(name)] = static_cast<float>(weight);
    return result;
  };
  const auto write = [&](std::size_t token, double speed_value,
                         double loudness_value) {
    speed[2 * token] = static_cast<float>(speed_value);
    speed[2 * token + 1] = static_cast<float>(speed_value);
    loudness[token] = static_cast<float>(loudness_value);
  };
  const auto write_blend = [&](std::size_t token,
                               const std::vector<float>& vector) {
    for (std::size_t row = 0; row < rows; ++row)
      blending[row * width + token] = vector[row];
  };

  const auto& front = segments.front();
  write(0, *front.start_speed, *front.start_loudness);
  write_blend(0, to_vector(*front.start_emotion));
  language_keys.front() = config.LanguageKey(languages.front());

  std::size_t cursor = 1;
  for (std::size_t index = 0; index < segments.size(); ++index) {
    const auto& segment = segments[index];
    const auto count = encoded[index].size();
    const auto start_vector = to_vector(*segment.start_emotion);
    const auto end_vector = to_vector(*segment.end_emotion);
    std::fill_n(language_keys.begin() + static_cast<std::ptrdiff_t>(cursor),
                count, config.LanguageKey(languages[index]));
    for (std::size_t token = 0; token < count; ++token) {
      const auto alpha = count == 1 ? 0.5
                                    : static_cast<double>(token) /
                                          static_cast<double>(count - 1);
      write(cursor + token, Lerp(*segment.start_speed, *segment.end_speed, alpha),
            Lerp(*segment.start_loudness, *segment.end_loudness, alpha));
      for (std::size_t row = 0; row < rows; ++row)
        blending[row * width + cursor + token] = static_cast<float>(
            start_vector[row] +
            (end_vector[row] - start_vector[row]) * alpha);
    }
    cursor += count;
  }

  const auto& back = segments.back();
  write(width - 1, *back.end_speed, *back.end_loudness);
  write_blend(width - 1, to_vector(*back.end_emotion));
  language_keys.back() = config.LanguageKey(languages.back());

  std::vector<std::int64_t> targets;
  std::size_t offset = 1;
  for (std::size_t index = 0; index < marker_tokens.size(); ++index) {
    for (const auto token : marker_tokens[index])
      targets.push_back(token + static_cast<std::int64_t>(offset));
    offset += encoded[index].size();
  }

  const auto size = static_cast<std::int64_t>(width);
  metagraph::TensorMap inputs;
  if (!targets.empty()) {
    const auto count = static_cast<std::int64_t>(targets.size());
    inputs["target_phoneme_indices"] =
        metagraph::Tensor::Int64({count}, std::move(targets));
  }
  inputs["phoneme_keys"] =
      metagraph::Tensor::Int64({1, size}, std::move(phonemes));
  inputs["actors"] =
      metagraph::Tensor::Int64({1, 1}, {config.character_key});
  inputs["languages"] =
      metagraph::Tensor::Int64({1, size}, std::move(language_keys));
  inputs["text_lengths"] = metagraph::Tensor::Int64({1}, {size});
  inputs["speed"] =
      metagraph::Tensor::Float32({1, 2 * size}, std::move(speed));
  inputs["loudness"] =
      metagraph::Tensor::Float32({1, 1, size}, std::move(loudness));

  // Without a named emotion the character graph supplies its own default.
  if (rows == 0) return inputs;
  std::vector<std::int64_t> emotions(rows * width);
  for (std::size_t row = 0; row < rows; ++row) {
    const auto key = config.EmotionKey(distinct[row]);
    std::fill(emotions.begin() + static_cast<std::ptrdiff_t>(row * width),
              emotions.begin() + static_cast<std::ptrdiff_t>((row + 1) * width),
              key);
  }
  const auto count = static_cast<std::int64_t>(rows);
  inputs["emotions"] =
      metagraph::Tensor::Int64({1, count, size}, std::move(emotions));
  inputs["emotions_blending"] =
      metagraph::Tensor::Float32({1, count, size}, std::move(blending));
  return inputs;
}

}  // namespace thespeon::inference_detail

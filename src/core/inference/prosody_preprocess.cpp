// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/prosody_preprocess.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace thespeon {
namespace {

template <typename Value>
using Curve = std::vector<std::pair<std::size_t, Value>>;

std::string Lowercase(std::string value) {
  for (auto& character : value)
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  return value;
}

double Lerp(const double& start, const double& end, double alpha) {
  return start + (end - start) * alpha;
}

// Coincident keypoints are a deliberate discontinuity: one segment's end and
// the next one's start. Starts read the left side, ends the right.
template <typename Value, typename Interpolate>
Value SampleCurve(const Curve<Value>& curve, std::size_t position,
                  bool sample_right_side, Interpolate&& interpolate) {
  if (position < curve.front().first) return curve.front().second;
  if (position > curve.back().first) return curve.back().second;

  std::size_t first = 0;
  while (first < curve.size() && curve[first].first < position) ++first;
  if (first < curve.size() && curve[first].first == position) {
    auto last = first;
    while (last + 1 < curve.size() && curve[last + 1].first == position) ++last;
    return curve[sample_right_side ? last : first].second;
  }

  const auto& low = curve[first - 1];
  const auto& high = curve[first];
  const auto alpha = static_cast<double>(position - low.first) /
                     static_cast<double>(high.first - low.first);
  return interpolate(low.second, high.second, alpha);
}

template <typename Value, typename Sanitize, typename Interpolate>
void PopulateCurve(std::vector<SynthInputSegmentV100>& segments,
                   std::optional<Value> SynthInputSegmentV100::*start_field,
                   std::optional<Value> SynthInputSegmentV100::*end_field,
                   const Value& unset, const std::optional<Value>& fallback,
                   const char* name, Sanitize&& sanitize,
                   Interpolate&& interpolate) {
  Curve<Value> curve;
  std::vector<std::pair<std::size_t, std::size_t>> positions;

  std::size_t global_position = 0;
  for (auto& segment : segments) {
    if (segment.text.empty())
      throw std::runtime_error(std::string("Cannot populate ") + name +
                               " keypoints for an empty segment");
    const auto start = global_position;
    const auto end = start + segment.text.size() - 1;
    positions.emplace_back(start, end);

    auto start_value = segment.*start_field ? *(segment.*start_field) : unset;
    if (sanitize(start_value)) curve.emplace_back(start, start_value);
    segment.*start_field = std::move(start_value);

    auto end_value = segment.*end_field ? *(segment.*end_field) : unset;
    if (sanitize(end_value)) curve.emplace_back(end, end_value);
    segment.*end_field = std::move(end_value);

    global_position += segment.text.size();
  }

  if (curve.empty()) {
    if (!fallback) return;
    curve.emplace_back(0, *fallback);
  }

  for (std::size_t index = 0; index < segments.size(); ++index) {
    segments[index].*start_field = SampleCurve<Value>(
        curve, positions[index].first, false, interpolate);
    segments[index].*end_field = SampleCurve<Value>(
        curve, positions[index].second, true, interpolate);
  }
}

}  // namespace

bool SanitizeEmotionBlend(EmotionWeights& blend) {
  EmotionWeights folded;
  for (const auto& [name, weight] : blend) {
    auto key = Lowercase(name);
    if (key == "none") continue;
    folded[std::move(key)] += weight;
  }

  double sum = 0.0;
  for (auto entry = folded.begin(); entry != folded.end();) {
    auto weight = entry->second;
    if (std::isnan(weight))
      weight = 0.0;
    else if (std::isinf(weight))
      weight = weight > 0.0 ? 1.0 : 0.0;
    else
      weight = std::clamp(weight, 0.0, 1.0);

    if (weight <= 0.0) {
      entry = folded.erase(entry);
      continue;
    }
    entry->second = weight;
    sum += weight;
    ++entry;
  }

  if (sum <= 0.0) {
    blend.clear();
    return false;
  }
  for (auto& entry : folded) entry.second /= sum;
  blend = std::move(folded);
  return true;
}

EmotionWeights InterpolateEmotionBlends(const EmotionWeights& start,
                                        const EmotionWeights& end,
                                        double alpha) {
  const auto clamped = std::clamp(alpha, 0.0, 1.0);
  EmotionWeights result;
  for (const auto& [name, weight] : start) result[name] = weight * (1.0 - clamped);
  for (const auto& [name, weight] : end) result[name] += weight * clamped;
  SanitizeEmotionBlend(result);
  return result;
}

void PopulateEmotionKeypoints(
    std::vector<SynthInputSegmentV100>& segments,
    const std::optional<std::string>& default_emotion) {
  std::optional<EmotionWeights> fallback;
  if (default_emotion) {
    EmotionWeights blend{{Lowercase(*default_emotion), 1.0}};
    if (SanitizeEmotionBlend(blend)) fallback = std::move(blend);
  }
  PopulateCurve<EmotionWeights>(
      segments, &SynthInputSegmentV100::start_emotion,
      &SynthInputSegmentV100::end_emotion, EmotionWeights{}, fallback,
      "emotion", [](EmotionWeights& value) { return SanitizeEmotionBlend(value); },
      InterpolateEmotionBlends);
}

void PopulateSpeedKeypoints(std::vector<SynthInputSegmentV100>& segments) {
  PopulateCurve<double>(segments, &SynthInputSegmentV100::start_speed,
                        &SynthInputSegmentV100::end_speed, 1.0,
                        std::optional<double>(1.0), "speed",
                        [](double&) { return true; }, Lerp);
}

void PopulateLoudnessKeypoints(std::vector<SynthInputSegmentV100>& segments) {
  PopulateCurve<double>(segments, &SynthInputSegmentV100::start_loudness,
                        &SynthInputSegmentV100::end_loudness, 1.0,
                        std::optional<double>(1.0), "loudness",
                        [](double&) { return true; }, Lerp);
}

}  // namespace thespeon

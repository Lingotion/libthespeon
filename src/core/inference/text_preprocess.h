// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/input/thespeon_input.h"
#include "core/module/module.h"
#include "textprep_runner.h"

namespace thespeon::inference_detail {

// A language pack's text preprocessing rules (its .textprep file).
using TextRules = metagraph::textprep::TextPreprocessing;

std::u32string DecodeUtf8(const std::string& value);
std::string EncodeUtf8(char32_t code_point);

struct NormalizedText {
  std::string text;
  std::vector<std::size_t> markers;
};

// Without rules, natural text is left as it is and only the checks that do not
// depend on language are made.
NormalizedText NormalizeSegment(const std::string& text, bool keep_leading,
                                bool keep_trailing, bool custom_pronounced,
                                std::size_t segment_index,
                                const TextRules* rules);

std::vector<std::string> UnknownWords(
    const std::vector<SynthInputSegmentV100>& segments,
    const std::unordered_map<std::string, std::string>& lookup,
    const TextRules& rules);

std::vector<std::int64_t> EncodeForCharacter(
    const std::string& text, const std::vector<std::size_t>& markers,
    const std::unordered_map<std::string, std::string>& lookup,
    const TextRules& rules, const CharacterConfig& config,
    bool custom_pronounced,
    std::vector<std::int64_t>* marker_tokens,
    std::vector<std::string>* warnings);

}  // namespace thespeon::inference_detail

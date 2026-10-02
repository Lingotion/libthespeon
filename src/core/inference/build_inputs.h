// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/input/thespeon_input.h"
#include "core/module/module.h"
#include "metagraph_runner.h"

namespace thespeon::inference_detail {

metagraph::TensorMap BuildLanguageInputs(
    const std::vector<std::string>& words, const LanguageConfig& config);

void AddLanguageResults(
    const std::vector<std::string>& words, const metagraph::Tensor& result,
    const LanguageConfig& config,
    std::unordered_map<std::string, std::string>& lookup);

metagraph::TensorMap BuildCharacterInputs(
    const std::vector<SynthInputSegmentV100>& segments,
    const std::vector<std::vector<std::int64_t>>& encoded,
    const std::vector<std::vector<std::int64_t>>& marker_tokens,
    const CharacterConfig& config, const std::vector<std::string>& languages);

}  // namespace thespeon::inference_detail

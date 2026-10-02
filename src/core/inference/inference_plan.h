// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/inference/inference_runtime.h"
#include "core/input/thespeon_input.h"
#include "core/module/module.h"

namespace thespeon {

// Throws the input errors InferencePlan's constructor would, loading nothing.
void ValidatePlanInput(const CharacterModule& character,
                       const ThespeonInput& input);

// Prepares one language request per segment language that has words missing
// from its lookup table, followed by the character request. The language
// results are applied between those two stages.
class InferencePlan {
 public:
  // language_modules maps each SegmentLanguages code to its module.
  InferencePlan(
      const CharacterModule& character,
      const std::unordered_map<std::string, LanguageModule>& language_modules,
      std::filesystem::path binaries, ThespeonInput input);
  ~InferencePlan();

  InferencePlan(InferencePlan&&) noexcept;
  InferencePlan& operator=(InferencePlan&&) noexcept;
  InferencePlan(const InferencePlan&) = delete;
  InferencePlan& operator=(const InferencePlan&) = delete;

  std::vector<InferenceRequest> PrepareLanguageModules() const;
  // One result per request, in the order PrepareLanguageModules returned them.
  void ApplyLanguageModuleResults(
      const std::vector<metagraph::Tensor>& results);
  InferenceRequest PrepareCharacterModule(
      std::vector<std::string>* warnings = nullptr) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace thespeon

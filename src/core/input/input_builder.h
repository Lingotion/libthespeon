// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/input/thespeon_input.h"

namespace thespeon {

enum class Endpoint { Start, End };

// Assembles a synthesis-input document; schema rules are checked by ToJson.
class InputBuilder {
 public:
  InputBuilder();
  // Throws unless json is a valid document.
  static InputBuilder FromJson(std::string_view json);

  void SetCharacterModule(const std::string& identifier);
  void SetCharacter(const std::string& name,
                    const std::optional<std::string>& module_type,
                    const std::optional<std::string>& module_version);
  void SetDefaultEmotion(const std::optional<std::string>& emotion);
  void SetDefaultLanguage(const ModuleLanguage& language);

  std::size_t AddSegment(const std::string& text);
  void SetCustomPronounced(std::size_t segment, bool enabled);
  void SetSegmentLanguage(std::size_t segment,
                          const std::optional<ModuleLanguage>& language);
  void SetSegmentEmotion(std::size_t segment,
                         const std::optional<std::string>& emotion);
  void AddSegmentEmotion(std::size_t segment, Endpoint at,
                         const std::string& emotion, double weight);
  void SetSegmentSpeed(std::size_t segment, Endpoint at, double speed);
  void SetSegmentLoudness(std::size_t segment, Endpoint at, double loudness);

  ThespeonInput Build() const;
  std::string ToJson() const;

 private:
  nlohmann::json& Segment(std::size_t index);

  nlohmann::json document_;
};

}  // namespace thespeon

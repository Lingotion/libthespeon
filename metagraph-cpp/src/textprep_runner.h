#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lingotion::textpreprocessing {
class TextPreprocessing;
}

namespace metagraph::textprep {

// The major version of text_preprocessing.proto this runner reads.
inline constexpr unsigned kSupportedMajorVersion = 3;

// The runtimes' audio sample request marker, which keep_only never removes.
inline constexpr char32_t kAudioSampleRequest = 0x25ce;

// A port of model-meta-graph's textprep_runner.py: runs a .textprep file's
// steps, number forms, rule sets and word definition. Everything Unicode is
// read from the file; nothing here uses a regex engine or platform Unicode.
// Text is a sequence of code points, and every index is a code point index.
class TextPreprocessing {
 public:
  // Throws std::runtime_error, naming the file, if it cannot be read or its
  // rules are malformed.
  static TextPreprocessing Load(const std::filesystem::path& path);

  explicit TextPreprocessing(
      const lingotion::textpreprocessing::TextPreprocessing& message);
  ~TextPreprocessing();
  TextPreprocessing(TextPreprocessing&&) noexcept;
  TextPreprocessing& operator=(TextPreprocessing&&) noexcept;
  TextPreprocessing(const TextPreprocessing&) = delete;
  TextPreprocessing& operator=(const TextPreprocessing&) = delete;

  const std::string& iso639_2() const;

  std::u32string ApplySteps(std::u32string text) const;

  // Splits text into (part, is_number) pairs, alternating text and numbers.
  std::vector<std::pair<std::u32string, bool>> Partition(
      const std::u32string& text) const;

  // Speaks out a number, as matched by Partition, in phonemes.
  std::u32string Expand(const std::u32string& number) const;

  // The words of text as (index, word) pairs. is_known says whether a word is
  // in the lookup table; it decides whether joiners right after a word stay.
  std::vector<std::pair<std::size_t, std::u32string>> SplitWords(
      const std::u32string& text,
      const std::function<bool(const std::u32string&)>& is_known = {}) const;

  // Whether code_point is in the named character class; throws for an
  // unknown class.
  bool InClass(std::string_view name, char32_t code_point) const;

  // What the first lowercase step makes of code_point on its own, without
  // context; code_point itself when there is no lowercase step.
  std::u32string LowerCodePoint(char32_t code_point) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace metagraph::textprep

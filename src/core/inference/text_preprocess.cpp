// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/text_preprocess.h"

#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/inference/control_characters.h"
#include "core/inference/unicode_case.h"

namespace thespeon::inference_detail {
namespace {

bool IsWordStart(char32_t value) {
  return unicode::IsLetter(value) || value == U'\'';
}

// A combining mark extends the word it follows but never starts one, so a stray
// mark after punctuation stays a symbol rather than becoming a word of pure
// diacritics. Without this, lowercased İ (i + U+0307) and decomposed accents
// split their word in two. Python's service splits on isalpha alone.
bool IsWordContinuation(char32_t value) {
  return IsWordStart(value) || unicode::IsMark(value);
}

std::string EncodeUtf8String(const std::u32string& value) {
  std::string result;
  for (const auto code_point : value) result += EncodeUtf8(code_point);
  return result;
}

std::vector<std::string> SplitWords(const std::string& text) {
  const auto value = DecodeUtf8(text);
  std::vector<std::string> result;
  for (std::size_t index = 0; index < value.size();) {
    if (!IsWordStart(value[index])) {
      ++index;
      continue;
    }
    const auto start = index;
    while (index < value.size() && IsWordContinuation(value[index])) ++index;
    result.push_back(EncodeUtf8String(value.substr(start, index - start)));
  }
  return result;
}

// The set the Python, Unity and Unreal preprocessors all fold. Kept one per line
// with the Unicode names so it stays diffable by eye against the other three.
bool IsAmbiguousApostrophe(char32_t value) {
  switch (value) {
    case 0x2018:  // LEFT SINGLE QUOTATION MARK
    case 0x2019:  // RIGHT SINGLE QUOTATION MARK
    case 0x201b:  // SINGLE HIGH-REVERSED-9 QUOTATION MARK
    case 0x02bc:  // MODIFIER LETTER APOSTROPHE
    case 0x02bb:  // MODIFIER LETTER TURNED COMMA
    case 0xff07:  // FULLWIDTH APOSTROPHE
    case 0x0060:  // GRAVE ACCENT
    case 0x00b4:  // ACUTE ACCENT
    case 0x2032:  // PRIME
    case 0x275b:  // HEAVY SINGLE TURNED COMMA QUOTATION MARK ORNAMENT
    case 0x275c:  // HEAVY SINGLE COMMA QUOTATION MARK ORNAMENT
    case 0x02c8:  // MODIFIER LETTER VERTICAL LINE
    case 0x02ca:  // MODIFIER LETTER ACUTE ACCENT
    case 0x02cb:  // MODIFIER LETTER GRAVE ACCENT
    case 0x1fef:  // GREEK VARIA
    case 0x1ffd:  // GREEK OXIA
    case 0x1fbf:  // GREEK PSILI
    case 0x1ffe:  // GREEK DASIA
    case 0x0374:  // GREEK NUMERAL SIGN
    case 0x0384:  // GREEK TONOS
    case 0x055a:  // ARMENIAN APOSTROPHE
    case 0x07f4:  // NKO HIGH TONE APOSTROPHE
    case 0x07f5:  // NKO LOW TONE APOSTROPHE
    case 0x05f3:  // HEBREW PUNCTUATION GERESH
    case 0x05f4:  // HEBREW PUNCTUATION GERSHAYIM
    case 0xfe32:  // PRESENTATION FORM FOR VERTICAL EN DASH
      return true;
    default:
      return false;
  }
}

// Grapheme cleanup. Only graphemic text goes through here - see NormalizeSegment.
std::string NormalizeText(const std::string& text, bool keep_leading,
                          bool keep_trailing) {
  std::u32string result;
  bool previous_space = !keep_leading;
  for (auto code_point : DecodeUtf8(text)) {
    if (IsAmbiguousApostrophe(code_point)) code_point = U'\'';

    if (unicode::IsSpace(code_point)) {
      if (!previous_space) result.push_back(U' ');
      previous_space = true;
    } else {
      result.push_back(code_point);
      previous_space = false;
    }
  }
  if (!keep_trailing && !result.empty() && result.back() == U' ')
    result.pop_back();
  return EncodeUtf8String(unicode::ToLower(result));
}

// Runs on both graphemic and custom-pronounced text, so it is the single place
// sample requests are collapsed. Adjacent requests mean one sample; any
// character between them, a space included, separates two.
NormalizedText ExtractMarkers(const std::string& text, std::size_t index) {
  std::u32string kept;
  std::vector<std::size_t> positions;
  bool previous_marker = false;
  for (const auto code_point : DecodeUtf8(text)) {
    if (code_point == control::kSequenceStart ||
        code_point == control::kSequenceEnd)
      throw std::invalid_argument(
          "$.segments[" + std::to_string(index) +
          "].text: Reserved sequence-marker character");
    if (code_point == control::kAudioSampleRequest) {
      if (!previous_marker) positions.push_back(kept.size());
      previous_marker = true;
      continue;
    }
    previous_marker = false;
    kept.push_back(code_point);
  }
  return {EncodeUtf8String(kept), std::move(positions)};
}

}  // namespace

std::u32string DecodeUtf8(const std::string& value) {
  std::u32string result;
  for (std::size_t index = 0; index < value.size();) {
    const auto lead = static_cast<unsigned char>(value[index]);
    if (lead < 0x80) {
      result.push_back(lead);
      ++index;
      continue;
    }

    int continuation_count = 0;
    char32_t code_point = 0;
    if ((lead & 0xe0) == 0xc0) {
      continuation_count = 1;
      code_point = lead & 0x1f;
    } else if ((lead & 0xf0) == 0xe0) {
      continuation_count = 2;
      code_point = lead & 0x0f;
    } else if ((lead & 0xf8) == 0xf0) {
      continuation_count = 3;
      code_point = lead & 0x07;
    } else {
      result.push_back(0xfffd);
      ++index;
      continue;
    }

    if (index + static_cast<std::size_t>(continuation_count) >= value.size()) {
      result.push_back(0xfffd);
      ++index;
      continue;
    }
    bool valid = true;
    for (int offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xc0) != 0x80) {
        valid = false;
        break;
      }
      code_point = (code_point << 6) | (continuation & 0x3f);
    }
    if (!valid || code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
      result.push_back(0xfffd);
      ++index;
    } else {
      result.push_back(code_point);
      index += static_cast<std::size_t>(continuation_count + 1);
    }
  }
  return result;
}

std::string EncodeUtf8(char32_t code_point) {
  std::string result;
  if (code_point < 0x80) {
    result.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    result.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    result.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point < 0x10000) {
    result.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    result.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    result.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
  return result;
}

NormalizedText NormalizeSegment(const std::string& text, bool keep_leading,
                                bool keep_trailing, bool custom_pronounced,
                                std::size_t segment_index) {
  // Custom-pronounced segments carry a phonetic spelling verbatim, so they skip
  // grapheme cleanup entirely, as the Python service does. Sample requests are
  // still extracted from them and the reserved sequence markers still rejected.
  //
  // The accepted consequence is that a typed U+0027 apostrophe stays U+0027 and
  // encodes as its own id rather than as U+02C8 primary stress. The user is
  // spelling phonemes and is expected to type the phoneme they mean.
  return ExtractMarkers(
      custom_pronounced ? text
                        : NormalizeText(text, keep_leading, keep_trailing),
      segment_index);
}

std::vector<std::string> UnknownWords(
    const std::vector<SynthInputSegmentV100>& segments,
    const std::unordered_map<std::string, std::string>& lookup) {
  std::vector<std::string> result;
  std::set<std::string> seen;
  for (const auto& segment : segments) {
    if (segment.is_custom_pronounced) continue;
    for (const auto& word : SplitWords(segment.text)) {
      if (!lookup.count(word) && seen.insert(word).second)
        result.push_back(word);
    }
  }
  return result;
}

std::vector<std::int64_t> EncodeForCharacter(
    const std::string& text, const std::vector<std::size_t>& markers,
    const std::unordered_map<std::string, std::string>& lookup,
    const CharacterConfig& config, bool custom_pronounced,
    std::vector<std::int64_t>* marker_tokens,
    std::vector<std::string>* warnings) {
  const auto input = DecodeUtf8(text);
  std::vector<std::int64_t> result;
  std::size_t next_marker = 0;
  const auto append = [&](char32_t code_point) {
    const auto symbol = EncodeUtf8(code_point);
    const auto found = config.phoneme_ids.find(symbol);
    if (found != config.phoneme_ids.end()) {
      result.push_back(found->second);
    } else if (warnings != nullptr) {
      warnings->push_back("Skipping unsupported symbol \"" + symbol + "\"");
    }
  };
  const auto emit = [&](std::size_t token) {
    if (marker_tokens != nullptr)
      marker_tokens->push_back(static_cast<std::int64_t>(token));
    ++next_marker;
  };

  for (std::size_t index = 0; index < input.size();) {
    if (!custom_pronounced && IsWordStart(input[index])) {
      const auto start = index;
      const auto token_start = result.size();
      while (index < input.size() && IsWordContinuation(input[index])) {
        ++index;
      }
      const auto word = EncodeUtf8String(input.substr(start, index - start));
      const auto found = lookup.find(word);
      if (found == lookup.end())
        throw std::runtime_error("No pronunciation for \"" + word + "\"");
      for (const auto code_point : DecodeUtf8(found->second)) append(code_point);
      const auto graphemes = index - start;
      const auto tokens = result.size() - token_start;
      while (next_marker < markers.size() && markers[next_marker] < index) {
        const auto scaled = (markers[next_marker] - start) * tokens;
        emit(token_start + (scaled + graphemes / 2) / graphemes);
      }
    } else {
      while (next_marker < markers.size() && markers[next_marker] == index) {
        emit(result.size());
      }
      append(input[index]);
      ++index;
    }
  }
  while (next_marker < markers.size()) emit(result.size());
  return result;
}

}  // namespace thespeon::inference_detail

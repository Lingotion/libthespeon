// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/text_preprocess.h"

#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/inference/control_characters.h"

namespace thespeon::inference_detail {
namespace {

std::string EncodeUtf8String(const std::u32string& value) {
  std::string result;
  for (const auto code_point : value) result += EncodeUtf8(code_point);
  return result;
}

void RejectSequenceMarkers(const std::string& text, std::size_t index) {
  for (const auto code_point : DecodeUtf8(text)) {
    if (code_point == control::kSequenceStart ||
        code_point == control::kSequenceEnd)
      throw std::invalid_argument(
          "$.segments[" + std::to_string(index) +
          "].text: Reserved sequence-marker character");
  }
}

// The language's rules, then the boundary spaces between segments.
std::string NormalizeText(const std::string& text, bool keep_leading,
                          bool keep_trailing, const TextRules& rules) {
  auto result = rules.ApplySteps(DecodeUtf8(text));
  if (!keep_leading) {
    const auto first = result.find_first_not_of(U' ');
    result.erase(0, first == std::u32string::npos ? result.size() : first);
  }
  if (!keep_trailing) {
    const auto last = result.find_last_not_of(U' ');
    result.erase(last == std::u32string::npos ? 0 : last + 1);
  }
  return EncodeUtf8String(result);
}

// Runs on both graphemic and custom-pronounced text, so it is the single place
// sample requests are collapsed. Adjacent requests mean one sample; any
// character between them, a space included, separates two.
NormalizedText ExtractMarkers(const std::string& text) {
  std::u32string kept;
  std::vector<std::size_t> positions;
  bool previous_marker = false;
  for (const auto code_point : DecodeUtf8(text)) {
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

// A span of graphemic text that is spoken as one unit: a word or a number.
struct Spoken {
  std::size_t start;
  std::size_t end;
  std::u32string text;
  bool is_number;
};

// The words and numbers of graphemic text, in order. Words are looked up in
// lookup, which also decides whether joiners may end them.
std::vector<Spoken> SpokenSpans(
    const std::u32string& text,
    const std::unordered_map<std::string, std::string>& lookup,
    const TextRules& rules) {
  const auto is_known = [&](const std::u32string& word) {
    return lookup.count(EncodeUtf8String(word)) > 0;
  };
  std::vector<Spoken> result;
  std::size_t offset = 0;
  for (const auto& [part, is_number] : rules.Partition(text)) {
    if (is_number) {
      result.push_back({offset, offset + part.size(), part, true});
    } else {
      for (auto& [index, word] : rules.SplitWords(part, is_known))
        result.push_back(
            {offset + index, offset + index + word.size(), std::move(word),
             false});
    }
    offset += part.size();
  }
  return result;
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
                                std::size_t segment_index,
                                const TextRules* rules) {
  // Checked before the rules run, since they may remove the markers.
  RejectSequenceMarkers(text, segment_index);
  // Custom-pronounced segments carry a phonetic spelling verbatim, so they skip
  // grapheme cleanup entirely, as the Python service does. Sample requests are
  // still extracted from them.
  //
  // The accepted consequence is that a typed U+0027 apostrophe stays U+0027 and
  // encodes as its own id rather than as U+02C8 primary stress. The user is
  // spelling phonemes and is expected to type the phoneme they mean.
  if (custom_pronounced || rules == nullptr) return ExtractMarkers(text);
  return ExtractMarkers(NormalizeText(text, keep_leading, keep_trailing, *rules));
}

std::vector<std::string> UnknownWords(
    const std::vector<SynthInputSegmentV100>& segments,
    const std::unordered_map<std::string, std::string>& lookup,
    const TextRules& rules) {
  std::vector<std::string> result;
  std::set<std::string> seen;
  for (const auto& segment : segments) {
    if (segment.is_custom_pronounced) continue;
    for (const auto& span : SpokenSpans(DecodeUtf8(segment.text), lookup, rules)) {
      if (span.is_number) continue;
      const auto word = EncodeUtf8String(span.text);
      if (!lookup.count(word) && seen.insert(word).second)
        result.push_back(word);
    }
  }
  return result;
}

std::vector<std::int64_t> EncodeForCharacter(
    const std::string& text, const std::vector<std::size_t>& markers,
    const std::unordered_map<std::string, std::string>& lookup,
    const TextRules& rules, const CharacterConfig& config,
    bool custom_pronounced, std::vector<std::int64_t>* marker_tokens,
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
  // Symbols up to end pass through as they are.
  const auto pass_through = [&](std::size_t from, std::size_t end) {
    for (auto index = from; index < end; ++index) {
      while (next_marker < markers.size() && markers[next_marker] == index)
        emit(result.size());
      append(input[index]);
    }
  };

  std::size_t index = 0;
  const auto spans = custom_pronounced
                         ? std::vector<Spoken>{}
                         : SpokenSpans(input, lookup, rules);
  for (const auto& span : spans) {
    pass_through(index, span.start);
    std::u32string phonemes;
    if (span.is_number) {
      phonemes = rules.Expand(span.text);
    } else {
      const auto word = EncodeUtf8String(span.text);
      const auto found = lookup.find(word);
      if (found == lookup.end())
        throw std::runtime_error("No pronunciation for \"" + word + "\"");
      phonemes = DecodeUtf8(found->second);
    }
    const auto token_start = result.size();
    for (const auto code_point : phonemes) append(code_point);
    // Markers inside the span keep their relative position in it.
    const auto graphemes = span.end - span.start;
    const auto tokens = result.size() - token_start;
    while (next_marker < markers.size() && markers[next_marker] < span.end) {
      const auto scaled = (markers[next_marker] - span.start) * tokens;
      emit(token_start + (scaled + graphemes / 2) / graphemes);
    }
    index = span.end;
  }
  pass_through(index, input.size());
  while (next_marker < markers.size()) emit(result.size());
  return result;
}

}  // namespace thespeon::inference_detail

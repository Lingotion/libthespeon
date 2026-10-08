#include "textprep_runner.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <variant>

#include "text_preprocessing.pb.h"

namespace metagraph::textprep {
namespace {

namespace proto = lingotion::textpreprocessing;

// Deep enough for any number up to 10^18 in any sensible rule set; a rule set
// that recurses further is looping.
constexpr int kMaxRuleDepth = 64;

std::u32string Decode(const std::string& value) {
  std::u32string result;
  for (std::size_t index = 0; index < value.size();) {
    const auto lead = static_cast<unsigned char>(value[index]);
    int count = 0;
    char32_t code_point = lead;
    if (lead >= 0xf0) {
      count = 3;
      code_point = lead & 0x07;
    } else if (lead >= 0xe0) {
      count = 2;
      code_point = lead & 0x0f;
    } else if (lead >= 0xc0) {
      count = 1;
      code_point = lead & 0x1f;
    }
    if (index + count >= value.size()) {
      result.push_back(0xfffd);
      break;
    }
    for (int offset = 1; offset <= count; ++offset)
      code_point = (code_point << 6) |
                   (static_cast<unsigned char>(value[index + offset]) & 0x3f);
    result.push_back(code_point);
    index += static_cast<std::size_t>(count) + 1;
  }
  return result;
}

std::string Encode(const std::u32string& value) {
  std::string result;
  for (const auto code_point : value) {
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
  }
  return result;
}

bool StartsWith(const std::u32string& text, std::size_t position,
                const std::u32string& prefix) {
  return text.size() - position >= prefix.size() &&
         text.compare(position, prefix.size(), prefix) == 0;
}

template <typename Repeated>
void CheckChunks(const Repeated& values, int size, const std::string& what) {
  if (values.size() % size)
    throw std::runtime_error(what + " must come in groups of " +
                             std::to_string(size) + ", got " +
                             std::to_string(values.size()) + " values.");
}

// A character class: ascending, disjoint inclusive ranges of code points.
class CodePointSet {
 public:
  CodePointSet(const std::string& name,
               const google::protobuf::RepeatedField<std::uint32_t>& ranges) {
    CheckChunks(ranges, 2, "Character class '" + name + "'");
    std::int64_t previous_last = -1;
    for (int index = 0; index < ranges.size(); index += 2) {
      const char32_t first = ranges[index];
      const char32_t last = ranges[index + 1];
      if (first > last || static_cast<std::int64_t>(first) <= previous_last)
        throw std::runtime_error("Character class '" + name +
                                 "': ranges must be ascending and disjoint.");
      previous_last = last;
      ranges_.emplace_back(first, last);
    }
  }

  const std::pair<char32_t, char32_t>* RangeOf(char32_t code_point) const {
    auto found = std::upper_bound(
        ranges_.begin(), ranges_.end(), code_point,
        [](char32_t value, const auto& range) { return value < range.first; });
    if (found == ranges_.begin()) return nullptr;
    --found;
    return code_point <= found->second ? &*found : nullptr;
  }

  bool Contains(char32_t code_point) const {
    return RangeOf(code_point) != nullptr;
  }

 private:
  std::vector<std::pair<char32_t, char32_t>> ranges_;
};

using Classes = std::unordered_map<std::string, CodePointSet>;

const CodePointSet& Class(const Classes& classes, const std::string& name) {
  const auto found = classes.find(name);
  if (found == classes.end())
    throw std::runtime_error("Unknown character class '" + name + "'.");
  return found->second;
}

class Lowercase {
 public:
  Lowercase(const proto::Lowercase& message, const Classes& classes) {
    CheckChunks(message.runs(), 4, "Lowercase runs");
    std::int64_t previous_last = -1;
    for (int index = 0; index < message.runs_size(); index += 4) {
      const Run run{message.runs(index), message.runs(index + 1),
                    message.runs(index + 2), message.runs(index + 3)};
      if (run.stride < 1 || run.first > run.last || run.first <= previous_last)
        throw std::runtime_error(
            "Lowercase runs must be ascending and disjoint, with a positive "
            "stride.");
      previous_last = run.last;
      runs_.push_back(run);
    }
    for (const auto& entry : message.special())
      special_[entry.code_point()] = Decode(entry.lowercase());
    for (const auto& entry : message.contextual())
      contextual_.emplace(
          entry.code_point(),
          Context{entry.final_form(), &Class(classes, entry.cased_class()),
                  &Class(classes, entry.ignorable_class())});
  }

  std::u32string Lower(char32_t code_point) const {
    const auto special = special_.find(code_point);
    if (special != special_.end()) return special->second;
    const std::int64_t value = code_point;
    auto found = std::upper_bound(
        runs_.begin(), runs_.end(), value,
        [](std::int64_t left, const Run& run) { return left < run.first; });
    if (found != runs_.begin()) {
      --found;
      if (value <= found->last && (value - found->first) % found->stride == 0)
        return std::u32string(1, static_cast<char32_t>(value + found->delta));
    }
    return std::u32string(1, code_point);
  }

  std::u32string operator()(const std::u32string& text) const {
    std::u32string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
      const auto context = contextual_.find(text[index]);
      if (context != contextual_.end() &&
          IsFinal(text, index, *context->second.cased,
                  *context->second.ignorable))
        result.push_back(context->second.final_form);
      else
        result += Lower(text[index]);
    }
    return result;
  }

 private:
  struct Run {
    std::int64_t first, last, stride, delta;
  };
  struct Context {
    char32_t final_form;
    const CodePointSet* cased;
    const CodePointSet* ignorable;
  };

  static bool IsFinal(const std::u32string& text, std::size_t index,
                      const CodePointSet& cased,
                      const CodePointSet& ignorable) {
    std::size_t before = index;
    while (before > 0 && ignorable.Contains(text[before - 1])) --before;
    if (before == 0 || !cased.Contains(text[before - 1])) return false;
    std::size_t after = index + 1;
    while (after < text.size() && ignorable.Contains(text[after])) ++after;
    return after == text.size() || !cased.Contains(text[after]);
  }

  std::vector<Run> runs_;
  std::unordered_map<char32_t, std::u32string> special_;
  std::unordered_map<char32_t, Context> contextual_;
};

class Compose {
 public:
  explicit Compose(const proto::Compose& message) {
    CheckChunks(message.pairs(), 3, "Compose pairs");
    for (int index = 0; index < message.pairs_size(); index += 3)
      pairs_[Key(message.pairs(index), message.pairs(index + 1))] =
          message.pairs(index + 2);
    CheckChunks(message.combining_classes(), 3, "Combining classes");
    std::int64_t previous_last = -1;
    for (int index = 0; index < message.combining_classes_size(); index += 3) {
      const Entry entry{message.combining_classes(index),
                        message.combining_classes(index + 1),
                        message.combining_classes(index + 2)};
      if (entry.first > entry.last ||
          static_cast<std::int64_t>(entry.first) <= previous_last)
        throw std::runtime_error(
            "Combining classes must be ascending and disjoint.");
      previous_last = entry.last;
      classes_.push_back(entry);
    }
  }

  std::uint32_t CombiningClass(char32_t code_point) const {
    auto found = std::upper_bound(
        classes_.begin(), classes_.end(), code_point,
        [](char32_t value, const Entry& entry) { return value < entry.first; });
    if (found == classes_.begin()) return 0;
    --found;
    return code_point <= found->last ? found->value : 0;
  }

  std::u32string operator()(const std::u32string& text) const {
    std::u32string result;
    std::optional<std::size_t> starter;          // the last starter in result
    std::optional<std::uint32_t> last_class;     // of the last kept after it
    for (const auto code_point : text) {
      const auto combining_class = CombiningClass(code_point);
      if (starter) {
        const bool blocked =
            last_class && (*last_class == 0 || *last_class >= combining_class);
        if (!blocked) {
          const auto found = pairs_.find(Key(result[*starter], code_point));
          if (found != pairs_.end()) {
            result[*starter] = found->second;
            continue;
          }
        }
      }
      result.push_back(code_point);
      if (combining_class == 0) {
        starter = result.size() - 1;
        last_class.reset();
      } else {
        last_class = combining_class;
      }
    }
    return result;
  }

 private:
  struct Entry {
    char32_t first, last;
    std::uint32_t value;
  };

  static std::uint64_t Key(char32_t first, char32_t second) {
    return static_cast<std::uint64_t>(first) << 32 | second;
  }

  std::unordered_map<std::uint64_t, char32_t> pairs_;
  std::vector<Entry> classes_;
};

class Words {
 public:
  Words(const proto::WordDefinition& message, const Classes& classes)
      : start_(&Class(classes, message.start_class())),
        continuation_(&Class(classes, message.continue_class())),
        joiner_(&Class(classes, message.joiner_class())),
        joiner_may_end_(message.joiner_may_end()) {}

  bool IsWordCharacter(char32_t code_point) const {
    return start_->Contains(code_point) || continuation_->Contains(code_point) ||
           joiner_->Contains(code_point);
  }

  std::vector<std::pair<std::size_t, std::u32string>> Split(
      const std::u32string& text,
      const std::function<bool(const std::u32string&)>& is_known) const {
    std::vector<std::pair<std::size_t, std::u32string>> words;
    std::size_t index = 0;
    while (index < text.size()) {
      if (!InWord(text, index)) {
        ++index;
        continue;
      }
      auto start = index;
      while (index < text.size() && InWord(text, index)) ++index;
      while (start < index && continuation_->Contains(text[start]) &&
             !start_->Contains(text[start]))
        ++start;
      auto end = index;
      if (joiner_may_end_ && is_known) {
        auto after = end;
        while (after < text.size() && joiner_->Contains(text[after])) ++after;
        if (after > end && is_known(text.substr(start, after - start)))
          end = after;
      }
      if (end > start) words.emplace_back(start, text.substr(start, end - start));
      index = std::max(index, end);
    }
    return words;
  }

 private:
  bool InWord(const std::u32string& text, std::size_t index) const {
    const auto code_point = text[index];
    if (start_->Contains(code_point) || continuation_->Contains(code_point))
      return true;
    // A joiner is a word character only between two characters of the word
    // classes.
    return joiner_->Contains(code_point) && index > 0 &&
           index + 1 < text.size() && IsWordCharacter(text[index - 1]) &&
           IsWordCharacter(text[index + 1]);
  }

  const CodePointSet* start_;
  const CodePointSet* continuation_;
  const CodePointSet* joiner_;
  bool joiner_may_end_;
};

// Non-negative integers as decimal digits without leading zeros, so numbers of
// any length format as the reference runner's Python integers do. Every
// divisor is a power of ten, so division is a split of the digits.
using Number = std::string;

Number Normalized(Number value) {
  const auto first = value.find_first_not_of('0');
  return first == Number::npos ? "0" : value.substr(first);
}

bool Less(const Number& left, const Number& right) {
  return left.size() != right.size() ? left.size() < right.size()
                                     : left < right;
}

struct Substitution {
  enum class Kind { Quotient, Remainder, Self } kind;
  std::string rule_set;  // empty for the rule's own set
};

using Token = std::variant<std::u32string, Substitution,
                           std::vector<std::variant<std::u32string, Substitution>>>;

struct Rule {
  Number base_value;
  std::size_t divisor_digits;  // the divisor is 10^divisor_digits
  std::vector<Token> tokens;
};

std::vector<Token> ParseRuleText(const std::string& utf8) {
  using Plain = std::variant<std::u32string, Substitution>;
  const auto text = Decode(utf8);
  std::vector<Token> tokens;
  std::optional<std::vector<Plain>> section;
  std::u32string literal;
  const auto fail = [&](const std::string& message) {
    return std::runtime_error("Rule '" + utf8 + "': " + message);
  };
  const auto push = [&](auto token) {
    if (section)
      section->emplace_back(std::move(token));
    else
      tokens.emplace_back(std::move(token));
  };
  const auto flush = [&] {
    if (!literal.empty()) push(std::exchange(literal, {}));
  };

  for (std::size_t index = 0; index < text.size();) {
    const auto c = text[index];
    if (c == U'<' || c == U'>' || c == U'=') {
      const auto end = text.find(c, index + 1);
      if (end == std::u32string::npos)
        throw fail("unclosed '" + Encode(std::u32string(1, c)) +
                   "' substitution.");
      const auto name = Encode(text.substr(index + 1, end - index - 1));
      if (!name.empty() && name[0] != '%')
        throw fail("'" + name + "' is not a rule set name.");
      if (c == U'=' && name.empty())
        throw fail(
            "'==' would format the number with its own rule and never end.");
      flush();
      const auto kind = c == U'<'   ? Substitution::Kind::Quotient
                        : c == U'>' ? Substitution::Kind::Remainder
                                    : Substitution::Kind::Self;
      push(Substitution{kind, name});
      index = end + 1;
    } else if (c == U'[') {
      if (section) throw fail("optional sections cannot be nested.");
      flush();
      section.emplace();
      ++index;
    } else if (c == U']') {
      if (!section) throw fail("']' without '['.");
      flush();
      tokens.emplace_back(std::move(*section));
      section.reset();
      ++index;
    } else {
      literal.push_back(c);
      ++index;
    }
  }
  if (section) throw fail("unclosed '['.");
  flush();
  return tokens;
}

template <typename Visit>
void ForEachSubstitution(const std::vector<Token>& tokens, Visit visit) {
  for (const auto& token : tokens) {
    if (const auto* substitution = std::get_if<Substitution>(&token)) {
      visit(*substitution);
    } else if (const auto* section = std::get_if<2>(&token)) {
      for (const auto& inner : *section)
        if (const auto* nested = std::get_if<Substitution>(&inner))
          visit(*nested);
    }
  }
}

class RuleSets {
 public:
  explicit RuleSets(
      const google::protobuf::RepeatedPtrField<proto::NumberRuleSet>& sets) {
    for (const auto& rule_set : sets) {
      const auto& name = rule_set.name();
      if (name.empty() || name[0] != '%')
        throw std::runtime_error("Rule set name '" + name +
                                 "' must start with '%'.");
      if (sets_.count(name))
        throw std::runtime_error("Rule set '" + name + "' is defined twice.");
      if (rule_set.rules().empty())
        throw std::runtime_error("Rule set '" + name + "' has no rules.");
      if (rule_set.rules(0).base_value() < 0)
        throw std::runtime_error("Rule set '" + name +
                                 "': negative base values are not supported.");
      std::vector<Rule> rules;
      for (int index = 0; index < rule_set.rules_size(); ++index) {
        const auto& rule = rule_set.rules(index);
        if (index > 0 &&
            rule.base_value() <= rule_set.rules(index - 1).base_value())
          throw std::runtime_error(
              "Rule set '" + name +
              "': rules must be in ascending order of base value (" +
              std::to_string(rule_set.rules(index - 1).base_value()) +
              " then " + std::to_string(rule.base_value()) + ").");
        auto base_value = std::to_string(rule.base_value());
        const auto divisor_digits = base_value.size() - 1;
        rules.push_back({std::move(base_value), divisor_digits,
                         ParseRuleText(rule.text())});
      }
      sets_.emplace(name, std::move(rules));
    }
    for (const auto& [name, rules] : sets_) {
      for (const auto& rule : rules) {
        ForEachSubstitution(rule.tokens, [&](const Substitution& substitution) {
          const auto where = "Rule set '" + name + "', rule " +
                             rule.base_value + ": ";
          if (!substitution.rule_set.empty() &&
              !sets_.count(substitution.rule_set))
            throw std::runtime_error(where + "unknown rule set '" +
                                     substitution.rule_set + "'.");
          if (substitution.kind == Substitution::Kind::Self &&
              substitution.rule_set == name)
            throw std::runtime_error(
                where + "'=' names its own rule set and would never end.");
        });
      }
    }
  }

  bool Contains(const std::string& name) const { return sets_.count(name) > 0; }

  // Spells out a non-negative integer with the named rule set.
  std::u32string Format(const Number& number, const std::string& rule_set,
                        int depth = 0) const {
    if (depth > kMaxRuleDepth)
      throw std::runtime_error("Rule set '" + rule_set + "' recursed more than " +
                               std::to_string(kMaxRuleDepth) +
                               " times formatting " + number + ".");
    const Rule* rule = nullptr;
    for (const auto& candidate : sets_.at(rule_set)) {
      if (Less(number, candidate.base_value)) break;
      rule = &candidate;
    }
    if (rule == nullptr)
      throw std::runtime_error("Rule set '" + rule_set + "' has no rule for " +
                               number + ".");
    const auto split = number.size() > rule->divisor_digits
                           ? number.size() - rule->divisor_digits
                           : 0;
    const auto quotient = Normalized(number.substr(0, split));
    const auto remainder = Normalized(number.substr(split));

    std::u32string result;
    const auto substitute = [&](const Substitution& substitution) {
      const auto& value =
          substitution.kind == Substitution::Kind::Quotient    ? quotient
          : substitution.kind == Substitution::Kind::Remainder ? remainder
                                                               : number;
      result += Format(value,
                       substitution.rule_set.empty() ? rule_set
                                                     : substitution.rule_set,
                       depth + 1);
    };
    for (const auto& token : rule->tokens) {
      if (const auto* text = std::get_if<std::u32string>(&token)) {
        result += *text;
      } else if (const auto* substitution = std::get_if<Substitution>(&token)) {
        substitute(*substitution);
      } else if (remainder != "0") {
        for (const auto& inner : std::get<2>(token)) {
          if (const auto* text = std::get_if<std::u32string>(&inner))
            result += *text;
          else
            substitute(std::get<Substitution>(inner));
        }
      }
    }
    return result;
  }

 private:
  std::map<std::string, std::vector<Rule>> sets_;
};

class NumberForm {
 public:
  NumberForm(const proto::NumberForm& message, const Classes& classes,
             const RuleSets& rule_sets)
      : template_(message.template_()) {
    for (const auto& element : message.elements()) {
      Element parsed;
      parsed.optional = element.optional();
      switch (element.kind_case()) {
        case proto::FormElement::kDigits:
          parsed.digits = &Class(classes, element.digits());
          break;
        case proto::FormElement::kLiteral:
          if (element.literal().empty()) throw Fail("empty literal.");
          parsed.options.push_back(Decode(element.literal()));
          break;
        case proto::FormElement::kOneOf:
          if (element.one_of().options().empty() ||
              std::any_of(element.one_of().options().begin(),
                          element.one_of().options().end(),
                          [](const auto& option) { return option.empty(); }))
            throw Fail("one_of needs non-empty options.");
          for (const auto& option : element.one_of().options())
            parsed.options.push_back(Decode(option));
          break;
        default:
          throw Fail("empty element.");
      }
      elements_.push_back(std::move(parsed));
    }
    if (elements_.empty())
      throw std::runtime_error("Number form '" + template_ +
                               "' has no elements.");
    ParseTemplate(rule_sets);
  }

  // The texts the elements matched, or nothing. The match ends where they end.
  std::optional<std::vector<std::u32string>> Match(const std::u32string& text,
                                                   std::size_t position) const {
    std::vector<std::u32string> matched;
    for (const auto& element : elements_) {
      std::size_t length = 0;
      if (element.digits != nullptr) {
        while (position + length < text.size() &&
               element.digits->Contains(text[position + length]))
          ++length;
      } else {
        for (const auto& option : element.options) {
          if (StartsWith(text, position, option)) {
            length = option.size();
            break;
          }
        }
      }
      if (length == 0 && !element.optional) return std::nullopt;
      matched.push_back(text.substr(position, length));
      position += length;
    }
    return matched;
  }

  std::u32string Render(const std::vector<std::u32string>& matched,
                        const RuleSets& rule_sets) const {
    std::u32string result;
    for (const auto& part : parts_) {
      if (const auto* text = std::get_if<std::u32string>(&part)) {
        result += *text;
        continue;
      }
      const auto& [element, rule_set, each] = std::get<Part>(part);
      const auto& digits = matched[element];
      if (digits.empty())
        throw std::runtime_error("Template '" + template_ + "': element " +
                                 std::to_string(element + 1) +
                                 " matched nothing.");
      Number value;
      for (const auto digit : digits) {
        const auto* range = elements_[element].digits->RangeOf(digit);
        if (range == nullptr)
          throw std::runtime_error("'" + Encode(std::u32string(1, digit)) +
                                   "' is not in the digit class.");
        value.push_back(static_cast<char>('0' + (digit - range->first) % 10));
      }
      if (each) {
        for (std::size_t index = 0; index < value.size(); ++index) {
          if (index > 0) result.push_back(U' ');
          result += rule_sets.Format(value.substr(index, 1), rule_set);
        }
      } else {
        result += rule_sets.Format(Normalized(value), rule_set);
      }
    }
    return result;
  }

 private:
  struct Element {
    const CodePointSet* digits = nullptr;  // set for a digit run
    std::vector<std::u32string> options;   // a literal's one, or a one_of's
    bool optional = false;
  };
  struct Part {
    std::size_t element;  // from 0
    std::string rule_set;
    bool each;
  };

  std::runtime_error Fail(const std::string& message) const {
    return std::runtime_error("Number form '" + template_ + "': " + message);
  }

  void ParseTemplate(const RuleSets& rule_sets) {
    const auto text = Decode(template_);
    const auto fail = [&](const std::string& message) {
      return std::runtime_error("Template '" + template_ + "': " + message);
    };
    std::u32string literal;
    for (std::size_t index = 0; index < text.size();) {
      const auto c = text[index];
      if (c == U'}') throw fail("'}' without '{'.");
      if (c != U'{') {
        literal.push_back(c);
        ++index;
        continue;
      }
      const auto end = text.find(U'}', index + 1);
      if (end == std::u32string::npos) throw fail("unclosed '{'.");
      const auto inner = Encode(text.substr(index + 1, end - index - 1));
      std::vector<std::string> fields;
      for (std::size_t start = 0;;) {
        const auto colon = inner.find(':', start);
        fields.push_back(inner.substr(start, colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
      }
      const auto& number = fields[0];
      if ((fields.size() != 2 && fields.size() != 3) || number.empty() ||
          !std::all_of(number.begin(), number.end(),
                       [](char d) { return d >= '0' && d <= '9'; }) ||
          (fields.size() == 3 && fields[2] != "each"))
        throw fail("malformed substitution '{" + inner + "}'.");
      const std::size_t element = number.size() > 9 ? 0 : std::stoul(number);
      if (element < 1 || element > elements_.size())
        throw fail("element " + number + " does not exist; the form has " +
                   std::to_string(elements_.size()) + ".");
      if (elements_[element - 1].digits == nullptr)
        throw fail("element " + number + " is not a digit run.");
      if (!rule_sets.Contains(fields[1]))
        throw fail("unknown rule set '" + fields[1] + "'.");
      if (!literal.empty()) parts_.emplace_back(std::exchange(literal, {}));
      parts_.emplace_back(Part{element - 1, fields[1], fields.size() == 3});
      index = end + 1;
    }
    if (!literal.empty()) parts_.emplace_back(std::move(literal));
  }

  std::string template_;
  std::vector<Element> elements_;
  std::vector<std::variant<std::u32string, Part>> parts_;
};

using Step = std::function<std::u32string(const std::u32string&)>;

}  // namespace

struct TextPreprocessing::Impl {
  explicit Impl(const proto::TextPreprocessing& message)
      : iso639_2(message.iso639_2()),
        classes(ReadClasses(message)),
        words(ReadWords(message, classes)),
        rule_sets(message.rule_sets()) {
    for (const auto& step : message.steps()) steps.push_back(CompileStep(step));
    for (const auto& form : message.number_forms())
      number_forms.emplace_back(form, classes, rule_sets);
  }

  static Classes ReadClasses(const proto::TextPreprocessing& message) {
    if (message.major_version() != kSupportedMajorVersion)
      throw std::runtime_error(
          "Text preprocessing version " +
          std::to_string(message.major_version()) + "." +
          std::to_string(message.minor_version()) + "." +
          std::to_string(message.patch_version()) +
          " is not supported; expected major version " +
          std::to_string(kSupportedMajorVersion) + ".");
    Classes result;
    for (const auto& character_class : message.character_classes()) {
      const auto& name = character_class.name();
      if (result.count(name))
        throw std::runtime_error("Character class '" + name +
                                 "' is defined twice.");
      result.emplace(name, CodePointSet(name, character_class.ranges()));
    }
    return result;
  }

  static Words ReadWords(const proto::TextPreprocessing& message,
                         const Classes& classes) {
    if (!message.has_words())
      throw std::runtime_error("The rules define no words.");
    return Words(message.words(), classes);
  }

  Step CompileStep(const proto::Step& step) {
    switch (step.kind_case()) {
      case proto::Step::kCompose: {
        auto compose = std::make_shared<Compose>(step.compose());
        return [compose](const std::u32string& text) {
          return (*compose)(text);
        };
      }
      case proto::Step::kLowercase: {
        auto lowercase = std::make_shared<Lowercase>(step.lowercase(), classes);
        if (!first_lowercase) first_lowercase = lowercase;
        return [lowercase](const std::u32string& text) {
          return (*lowercase)(text);
        };
      }
      case proto::Step::kCollapseWhitespace: {
        const auto* space =
            &Class(classes, step.collapse_whitespace().space_class());
        return [space](const std::u32string& text) {
          std::u32string result;
          bool previous_was_space = false;
          for (const auto code_point : text) {
            if (space->Contains(code_point)) {
              if (!previous_was_space) result.push_back(U' ');
              previous_was_space = true;
            } else {
              result.push_back(code_point);
              previous_was_space = false;
            }
          }
          return result;
        };
      }
      case proto::Step::kReplaceChars: {
        const auto characters = Decode(step.replace_chars().characters());
        const auto replacement = Decode(step.replace_chars().replacement());
        return [characters, replacement](const std::u32string& text) {
          std::u32string result;
          for (const auto code_point : text) {
            if (characters.find(code_point) != std::u32string::npos)
              result += replacement;
            else
              result.push_back(code_point);
          }
          return result;
        };
      }
      case proto::Step::kReplaceText:
        return CompileReplaceText(step.replace_text());
      case proto::Step::kKeepOnly: {
        const auto* keep = &Class(classes, step.keep_only().character_class());
        return [keep](const std::u32string& text) {
          std::u32string result;
          for (const auto code_point : text)
            if (keep->Contains(code_point) || code_point == kAudioSampleRequest)
              result.push_back(code_point);
          return result;
        };
      }
      default:
        throw std::runtime_error("Unknown or empty step.");
    }
  }

  Step CompileReplaceText(const proto::ReplaceText& message) {
    const auto target = Decode(message.text());
    const auto replacement = Decode(message.replacement());
    if (target.empty())
      throw std::runtime_error("replace_text needs a text to replace.");
    const auto whole_word = message.whole_word();
    const Words* word_definition = &words;
    return [=](const std::u32string& text) {
      std::u32string result;
      for (std::size_t index = 0; index < text.size();) {
        const auto end = index + target.size();
        if (StartsWith(text, index, target) &&
            (!whole_word ||
             ((index == 0 || !word_definition->IsWordCharacter(text[index - 1])) &&
              (end == text.size() ||
               !word_definition->IsWordCharacter(text[end]))))) {
          result += replacement;
          index = end;
        } else {
          result.push_back(text[index++]);
        }
      }
      return result;
    };
  }

  // The first number form matching at position, with what it matched.
  std::optional<std::pair<const NumberForm*, std::vector<std::u32string>>>
  MatchNumber(const std::u32string& text, std::size_t position) const {
    for (const auto& form : number_forms) {
      auto matched = form.Match(text, position);
      if (matched && Length(*matched) > 0)
        return std::make_pair(&form, std::move(*matched));
    }
    return std::nullopt;
  }

  static std::size_t Length(const std::vector<std::u32string>& matched) {
    std::size_t result = 0;
    for (const auto& text : matched) result += text.size();
    return result;
  }

  std::string iso639_2;
  Classes classes;
  Words words;
  std::vector<Step> steps;
  std::shared_ptr<const Lowercase> first_lowercase;
  RuleSets rule_sets;
  std::vector<NumberForm> number_forms;
};

TextPreprocessing TextPreprocessing::Load(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  proto::TextPreprocessing message;
  if (!stream || !message.ParseFromIstream(&stream))
    throw std::runtime_error("Could not read text preprocessing file: " +
                             path.string());
  try {
    return TextPreprocessing(message);
  } catch (const std::exception& error) {
    throw std::runtime_error(path.string() + ": " + error.what());
  }
}

TextPreprocessing::TextPreprocessing(const proto::TextPreprocessing& message)
    : impl_(std::make_unique<Impl>(message)) {}

TextPreprocessing::~TextPreprocessing() = default;
TextPreprocessing::TextPreprocessing(TextPreprocessing&&) noexcept = default;
TextPreprocessing& TextPreprocessing::operator=(TextPreprocessing&&) noexcept =
    default;

const std::string& TextPreprocessing::iso639_2() const {
  return impl_->iso639_2;
}

std::u32string TextPreprocessing::ApplySteps(std::u32string text) const {
  for (const auto& step : impl_->steps) text = step(text);
  return text;
}

std::vector<std::pair<std::u32string, bool>> TextPreprocessing::Partition(
    const std::u32string& text) const {
  std::vector<std::pair<std::u32string, bool>> parts;
  std::size_t last = 0;
  std::size_t position = 0;
  while (position < text.size()) {
    const auto found = impl_->MatchNumber(text, position);
    if (!found) {
      ++position;
      continue;
    }
    if (position > last)
      parts.emplace_back(text.substr(last, position - last), false);
    const auto length = Impl::Length(found->second);
    parts.emplace_back(text.substr(position, length), true);
    last = position = position + length;
  }
  if (last < text.size()) parts.emplace_back(text.substr(last), false);
  return parts;
}

std::u32string TextPreprocessing::Expand(const std::u32string& number) const {
  for (const auto& form : impl_->number_forms) {
    const auto matched = form.Match(number, 0);
    if (matched && Impl::Length(*matched) == number.size())
      return form.Render(*matched, impl_->rule_sets);
  }
  throw std::runtime_error("'" + Encode(number) +
                           "' is not written in any of the number forms.");
}

std::vector<std::pair<std::size_t, std::u32string>>
TextPreprocessing::SplitWords(
    const std::u32string& text,
    const std::function<bool(const std::u32string&)>& is_known) const {
  return impl_->words.Split(text, is_known);
}

bool TextPreprocessing::InClass(std::string_view name,
                                char32_t code_point) const {
  return Class(impl_->classes, std::string(name)).Contains(code_point);
}

std::u32string TextPreprocessing::LowerCodePoint(char32_t code_point) const {
  return impl_->first_lowercase ? impl_->first_lowercase->Lower(code_point)
                                : std::u32string(1, code_point);
}

}  // namespace metagraph::textprep

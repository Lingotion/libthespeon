#!/usr/bin/env python3
# This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
# The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

"""Generates src/core/inference/unicode_tables.h.

The tables are taken from this interpreter's own str.isalpha(), str.isspace(),
str.lower() and unicodedata.category(), so the C++ preprocessor matches the Python service exactly when
both are generated from, and run on, the same Python version. Regenerate when the
service moves to a Python with a different unicodedata.unidata_version:

    python3 tools/gen_unicode_tables.py src/core/inference/unicode_tables.h

The header also carries a fingerprint of Python's behaviour over every code
point. UnicodeTables.MatchPythonOnEveryCodePoint recomputes it through the C++
functions, so the tables and the lookup code are checked against Python itself,
not against each other.
"""

import sys
import unicodedata

CODE_POINTS = 0x110000
CAPITAL_SIGMA = "Σ"
FINAL_SIGMA = "ς"


def ranges(predicate):
    result, start = [], None
    for code_point in range(CODE_POINTS + 1):
        inside = code_point < CODE_POINTS and predicate(code_point)
        if inside and start is None:
            start = code_point
        elif not inside and start is not None:
            result.append((start, code_point - 1))
            start = None
    return result


def lowercase_runs():
    """Single-code-point lowercase mappings as (first, last, stride, delta).

    Stride 1 is a block that shifts as a whole; stride 2 is the alternating
    upper/lower pairs of Latin Extended-A and friends, where only every other
    code point maps."""
    mapping = {}
    for code_point in range(CODE_POINTS):
        lowered = chr(code_point).lower()
        if lowered == chr(code_point):
            continue
        if len(lowered) != 1:
            # Handled in code; fail loudly if a new Unicode version adds one.
            assert code_point == 0x0130, hex(code_point)
            continue
        mapping[code_point] = ord(lowered)

    runs, code_points, index = [], sorted(mapping), 0
    while index < len(code_points):
        first = code_points[index]
        delta = mapping[first] - first
        count, stride = 1, 1
        for candidate in (1, 2):
            length = 1
            while (index + length < len(code_points)
                   and code_points[index + length] == first + length * candidate
                   and mapping[code_points[index + length]]
                   - code_points[index + length] == delta):
                length += 1
            if length > count:
                count, stride = length, candidate
        runs.append((first, first + (count - 1) * stride, stride, delta))
        index += count
    return runs, len(mapping)


def sigma_context(code_point):
    """How a code point counts for Final_Sigma, probed from str.lower().

    Python skips Case_Ignorable code points and then asks whether the next one
    is Cased. Whether an ignorable is also cased never matters, because it is
    always skipped, so three classes are enough."""
    text = chr(code_point)
    if ("A" + text + CAPITAL_SIGMA).lower()[-1] != FINAL_SIGMA:
        return None
    if (text + CAPITAL_SIGMA).lower()[-1] == FINAL_SIGMA:
        return "cased"
    return "ignorable"


def fingerprint():
    """FNV-1a over Python's behaviour for every code point. Must stay in step
    with PythonFingerprint in tests/text_preprocess_test.cpp."""
    value = 0xcbf29ce484222325

    def mix(word):
        nonlocal value
        for shift in (0, 8, 16, 24):
            value ^= (word >> shift) & 0xff
            value = (value * 0x100000001b3) & 0xffffffffffffffff

    for code_point in range(CODE_POINTS):
        text = chr(code_point)
        mix(int(text.isalpha()) | int(text.isspace()) << 1
            | int(unicodedata.category(text).startswith("M")) << 2)
        lowered = text.lower()
        mix(len(lowered))
        for character in lowered:
            mix(ord(character))
        mix(int(("A" + text + CAPITAL_SIGMA).lower()[-1] == FINAL_SIGMA)
            | int((text + CAPITAL_SIGMA).lower()[-1] == FINAL_SIGMA) << 1
            | int(("A" + CAPITAL_SIGMA + text).lower()[1] == FINAL_SIGMA) << 2)
    return value


def format_ranges(name, comment, values):
    rows = "\n".join("    {0x%05x, 0x%05x}," % value for value in values)
    return (f"// {comment} {len(values)} ranges.\n"
            f"inline constexpr CodePointRange {name}[] = {{\n{rows}\n}};\n")


def main():
    letters = ranges(lambda c: chr(c).isalpha())
    spaces = ranges(lambda c: chr(c).isspace())
    marks = ranges(lambda c: unicodedata.category(chr(c)).startswith("M"))
    ignorable = ranges(lambda c: sigma_context(c) == "ignorable")
    cased = ranges(lambda c: sigma_context(c) == "cased")
    runs, mapped = lowercase_runs()
    lower_rows = "\n".join("    {0x%05x, 0x%05x, %d, %d}," % run for run in runs)

    header = f"""#pragma once

// GENERATED by tools/gen_unicode_tables.py - do not edit, regenerate.
// Python {sys.version.split()[0]}, Unicode {unicodedata.unidata_version}.

#include <cstdint>

namespace thespeon::unicode {{

inline constexpr char kUnicodeVersion[] = "{unicodedata.unidata_version}";
inline constexpr std::uint64_t kPythonFingerprint = 0x{fingerprint():016x}ull;

struct CodePointRange {{
  char32_t first, last;
}};

struct LowercaseRun {{
  char32_t first, last;
  std::uint8_t stride;
  std::int32_t delta;
}};

{format_ranges("kLetters", "str.isalpha(): general category L.", letters)}
{format_ranges("kSpaces", "str.isspace().", spaces)}
{format_ranges("kMarks", "General category M (Mn, Mc, Me): combining marks.", marks)}
// str.lower() for the {mapped} code points with a single-code-point lowercase,
// in {len(runs)} runs. U+0130 is the only multi-code-point lowercase and is
// handled in code, as is final sigma.
inline constexpr LowercaseRun kLowercase[] = {{
{lower_rows}
}};

// Final-sigma context, as str.lower() applies it.
{format_ranges("kCaseIgnorable", "Skipped when looking for a neighbour:", ignorable)}
{format_ranges("kCased", "A cased neighbour:", cased)}
}}  // namespace thespeon::unicode
"""
    with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as output:
        output.write(header)


if __name__ == "__main__":
    main()

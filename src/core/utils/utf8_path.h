// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace thespeon {

// path.string() narrows through the ANSI code page on Windows; these keep
// every narrow path in UTF-8.
inline std::string PathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return std::string(value.begin(), value.end());
}

inline std::filesystem::path PathFromUtf8(std::string_view value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

}  // namespace thespeon

#pragma once

#include <filesystem>
#include <string>

namespace metagraph {

// path.string() narrows through the ANSI code page on Windows.
inline std::string PathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return std::string(value.begin(), value.end());
}

}  // namespace metagraph

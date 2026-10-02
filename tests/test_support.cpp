// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "test_support.h"

#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace thespeon::test {

TempDirectory::TempDirectory() {
  std::random_device device;
  std::mt19937_64 generator(device());
  const auto base = std::filesystem::temp_directory_path();
  for (int attempt = 0; attempt < 16; ++attempt) {
    auto candidate = base / ("thespeon-test-" + std::to_string(generator()));
    if (std::filesystem::create_directory(candidate)) {
      path_ = std::move(candidate);
      return;
    }
  }
  throw std::runtime_error("Could not create a temporary directory");
}

TempDirectory::~TempDirectory() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

void WriteFile(const std::filesystem::path& path,
               const std::string& contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) throw std::runtime_error("Could not write " + path.string());
  output << contents;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Could not read " + path.string());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

}  // namespace thespeon::test

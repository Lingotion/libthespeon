// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <filesystem>
#include <string>

namespace thespeon::test {

// A fresh directory under the system temp path, removed on destruction.
class TempDirectory {
 public:
  TempDirectory();
  ~TempDirectory();

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, const std::string& contents);
std::string ReadFile(const std::filesystem::path& path);

}  // namespace thespeon::test

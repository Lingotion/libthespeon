// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <cstddef>
#include <filesystem>

namespace thespeon {

struct ImportResult {
  std::size_t written = 0;
  std::size_t unchanged = 0;
};

class Importer {
 public:
  explicit Importer(std::filesystem::path data_directory);

  ImportResult Import(const std::filesystem::path& pack) const;

 private:
  std::filesystem::path data_directory_;
};

ImportResult ImportPack(const std::filesystem::path& pack,
                        const std::filesystem::path& data_directory);

}  // namespace thespeon

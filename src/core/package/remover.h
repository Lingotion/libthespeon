// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/module/catalog.h"

namespace thespeon {

struct RemoveResult {
  // The resolved target, as "Name" L v3.0.1, or "all modules".
  std::string target;
  std::size_t binaries_removed = 0;
  std::uintmax_t bytes_freed = 0;
  // Still named by a module that stays.
  std::size_t binaries_kept = 0;
  bool swept = true;
  // Configs that could not be read, which is what blocks the sweep.
  std::vector<std::string> problems;
  // Characters left with no compatible language module. Non-empty only for a
  // language target, and what force overrides.
  std::vector<std::string> stranded_characters;
};

class Remover {
 public:
  explicit Remover(std::filesystem::path data_directory);

  // A dry run resolves and computes exactly as a real one, minus the deletes,
  // so it doubles as the preview.
  RemoveResult Remove(const std::string& selector,
                      const std::string& module_type = {},
                      const Version& version = {}, bool force = false,
                      bool dry_run = false) const;
  RemoveResult RemoveAll(bool dry_run = false) const;

 private:
  std::filesystem::path data_directory_;
};

RemoveResult RemoveModel(const std::string& selector,
                         const std::filesystem::path& data_directory,
                         const std::string& module_type = {},
                         const Version& version = {}, bool force = false,
                         bool dry_run = false);
RemoveResult RemoveAllModels(const std::filesystem::path& data_directory,
                             bool dry_run = false);

}  // namespace thespeon

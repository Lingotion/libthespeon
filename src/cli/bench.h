// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <filesystem>
#include <string>

#include "core/input/thespeon_input.h"

namespace thespeon::cli {

void RunBenchmark(const std::filesystem::path& data_directory,
                  const ThespeonInput& input, int repeats);

// Times each inference option set and prints the one it recommends.
void RunSweep(const std::filesystem::path& data_directory,
              const ThespeonInput& input, int repeats);

}  // namespace thespeon::cli

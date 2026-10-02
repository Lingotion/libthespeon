// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/package/remover.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "session_cache.h"

namespace thespeon {
namespace {

namespace fs = std::filesystem;

std::string Describe(const CharacterModule& module) {
  std::string result = "\"" + module.character_name + "\" " +
                       ModuleTypeName(module.quality);
  if (module.version.IsValid()) result += " " + module.version.ToString();
  return result;
}

std::string Describe(const LanguageModule& module) {
  std::string result = module.identifier;
  if (module.version.IsValid()) result += " " + module.version.ToString();
  return result;
}

bool HasIdentifier(const CharacterModule& module, const std::string& value) {
  return !module.identifier.empty() && module.identifier == value;
}

bool HasIdentifier(const LanguageModule& module, const std::string& value) {
  return !module.identifier.empty() && module.identifier == value;
}

// The characters that would have no compatible language module left.
std::vector<std::string> StrandedCharacters(const Catalog& remaining) {
  std::vector<std::string> result;
  for (const auto& character : remaining.character_modules()) {
    std::string language;
    try {
      remaining.CompatibleLanguageModule(character, language);
    } catch (const std::exception&) {
      result.push_back(character.character_name);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::string JoinQuoted(const std::vector<std::string>& values) {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) result += ", ";
    result += "\"" + value + "\"";
  }
  return result;
}

// Every binary the remaining modules still name, including the ONNX models
// that appear only inside a metagraph rather than in a config's files list.
// Throws when a config or graph cannot be read, since its references would
// otherwise look unused.
std::set<fs::path> LiveBinaries(const Catalog& catalog,
                                const fs::path& binaries) {
  std::set<fs::path> live;
  std::vector<fs::path> graphs;
  const auto collect =
      [&](const std::unordered_map<std::string, FileReference>& files) {
        for (const auto& [name, reference] : files) {
          auto path = reference.Resolve(binaries);
          if (path.extension() == ".metagraph") graphs.push_back(path);
          live.insert(std::move(path));
        }
      };

  for (const auto& module : catalog.character_modules())
    collect(LoadCharacterConfig(module).files);
  for (const auto& module : catalog.language_modules())
    collect(LoadLanguageConfig(module).files);

  metagraph::GraphCache cache;
  for (const auto& graph : graphs) {
    if (!fs::exists(graph)) continue;
    const auto parsed = cache.Load(graph);
    for (const auto* node : metagraph::PreloadOrder(*parsed))
      live.insert(binaries / (node->model_path() + ".onnx"));
  }
  return live;
}

void Sweep(const fs::path& binaries, const std::set<fs::path>& live,
           bool dry_run, RemoveResult& result) {
  if (!fs::exists(binaries)) return;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(binaries)) {
    if (!entry.is_regular_file()) continue;
    const auto& path = entry.path();
    // Left behind by an interrupted import, which owns them.
    if (path.extension() == ".importing") continue;
    if (live.count(path) != 0) {
      ++result.binaries_kept;
      continue;
    }
    const auto size = fs::file_size(path, error);
    if (!dry_run && !fs::remove(path, error)) continue;
    ++result.binaries_removed;
    if (!error) result.bytes_freed += size;
  }
}

}  // namespace

Remover::Remover(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)) {}

RemoveResult Remover::Remove(const std::string& selector,
                             const std::string& module_type,
                             const Version& version, bool force,
                             bool dry_run) const {
  if (selector.empty()) throw std::runtime_error("Name a module to delete");

  const auto catalog = Catalog::Load(data_directory_);
  RemoveResult result;
  fs::path config_path;
  bool language_target = false;

  const CharacterModule* character_by_id = nullptr;
  const LanguageModule* language_by_id = nullptr;
  for (const auto& module : catalog.character_modules()) {
    if (HasIdentifier(module, selector)) character_by_id = &module;
  }
  for (const auto& module : catalog.language_modules()) {
    if (HasIdentifier(module, selector)) language_by_id = &module;
  }
  if (character_by_id != nullptr && language_by_id != nullptr)
    throw std::runtime_error("Module identifier " + selector +
                             " names both " +
                             character_by_id->config_path.filename().string() +
                             " and " +
                             language_by_id->config_path.filename().string());

  if (character_by_id != nullptr) {
    config_path = character_by_id->config_path;
    result.target = Describe(*character_by_id);
  } else if (language_by_id != nullptr) {
    if (!module_type.empty())
      throw std::runtime_error(
          "--module-type does not apply to language modules");
    config_path = language_by_id->config_path;
    result.target = Describe(*language_by_id);
    language_target = true;
  } else {
    // Only a character is named by anything but its identifier, so this is
    // also the path for an unknown selector, whose error it raises.
    const auto& module =
        catalog.SelectCharacter(selector, module_type, version);
    config_path = module.config_path;
    result.target = Describe(module);
  }

  const auto remaining = catalog.Without(config_path);
  if (language_target) {
    result.stranded_characters = StrandedCharacters(remaining);
    if (!result.stranded_characters.empty() && !force && !dry_run)
      throw std::runtime_error(
          "Deleting " + result.target + " would leave " +
          JoinQuoted(result.stranded_characters) +
          " with no language module; pass --force to delete anyway");
  }

  std::error_code error;
  if (!dry_run && !fs::remove(config_path, error))
    throw std::runtime_error("Could not delete " + config_path.string());

  if (!catalog.problems().empty()) {
    result.swept = false;
    result.problems = catalog.problems();
    return result;
  }

  const auto binaries = data_directory_ / "binaries";
  std::set<fs::path> live;
  try {
    live = LiveBinaries(remaining, binaries);
  } catch (const std::exception& problem) {
    result.swept = false;
    result.problems.emplace_back(problem.what());
    return result;
  }
  Sweep(binaries, live, dry_run, result);
  return result;
}

RemoveResult Remover::RemoveAll(bool dry_run) const {
  const auto catalog = Catalog::Load(data_directory_);
  RemoveResult result;
  result.target =
      "all " +
      std::to_string(catalog.character_modules().size() +
                     catalog.language_modules().size() +
                     catalog.problems().size()) +
      " module(s)";

  std::error_code error;
  for (const char* section : {"configs", "binaries"}) {
    const auto directory = data_directory_ / section;
    if (!fs::exists(directory)) continue;
    for (const auto& entry : fs::directory_iterator(directory)) {
      if (!entry.is_regular_file()) continue;
      const auto size = fs::file_size(entry.path(), error);
      if (!dry_run && !fs::remove(entry.path(), error)) continue;
      ++result.binaries_removed;
      if (!error) result.bytes_freed += size;
    }
    // Only if nothing unexpected is left in it.
    if (!dry_run) fs::remove(directory, error);
  }
  return result;
}

RemoveResult RemoveModel(const std::string& selector,
                         const std::filesystem::path& data_directory,
                         const std::string& module_type, const Version& version,
                         bool force, bool dry_run) {
  return Remover(data_directory)
      .Remove(selector, module_type, version, force, dry_run);
}

RemoveResult RemoveAllModels(const std::filesystem::path& data_directory,
                             bool dry_run) {
  return Remover(data_directory).RemoveAll(dry_run);
}

}  // namespace thespeon

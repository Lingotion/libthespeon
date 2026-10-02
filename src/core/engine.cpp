// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/engine.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/inference/inference_plan.h"
#include "core/inference/inference_runtime.h"
#include "core/inference/stream_buffer.h"
#include "core/utils/profile.h"
#include "core/utils/wav_writer.h"

namespace thespeon {
namespace {

namespace fs = std::filesystem;

// Long enough that the chunking loop runs every vocoder variant, and carries a
// word no lookup table holds so the phonemizer graph warms as well.
constexpr const char* kWarmupText =
    "Warming the voice up with a quiet sentence about zyrbalux and nothing "
    "else in particular.";

// Language a bare module identifier preloads and unloads, since it carries no
// segments to take languages from.
constexpr const char* kDefaultLanguage = "eng";

}  // namespace

fs::path DefaultDataDirectory() {
#if defined(_WIN32)
  if (const char* path = std::getenv("LOCALAPPDATA"))
    return fs::path(path) / "metagraph-cpp" / "models";
  if (const char* path = std::getenv("APPDATA"))
    return fs::path(path) / "metagraph-cpp" / "models";
#elif defined(__APPLE__)
  if (const char* path = std::getenv("HOME"))
    return fs::path(path) / "Library" / "Application Support" /
           "metagraph-cpp" / "models";
#else
  if (const char* path = std::getenv("XDG_DATA_HOME"))
    return fs::path(path) / "metagraph-cpp" / "models";
  if (const char* path = std::getenv("HOME"))
    return fs::path(path) / ".local" / "share" / "metagraph-cpp" / "models";
#endif
  throw std::runtime_error(
      "Cannot determine the user data directory; use --data-dir");
}

namespace {

std::string JoinFields(const std::vector<std::string>& fields) {
  std::string result;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index > 0) result += index + 1 == fields.size() ? " and " : ", ";
    result += fields[index];
  }
  return result;
}

SynthInputV100 PinnedTo(const CharacterModule& character) {
  SynthInputV100 result;
  if (!character.identifier.empty()) {
    result.module_identifier = character.identifier;
    return result;
  }
  result.character_name = character.character_name;
  result.module_type = ModuleTypeName(character.quality);
  result.module_version = character.version;
  return result;
}

std::vector<std::string> DistinctLanguages(const ThespeonInput& input) {
  std::vector<std::string> result;
  for (auto& language : SegmentLanguages(input)) {
    if (std::find(result.begin(), result.end(), language) == result.end())
      result.push_back(std::move(language));
  }
  return result;
}

const CharacterModule& CharacterByIdentifier(const Catalog& catalog,
                                             const std::string& identifier) {
  if (const auto* character = catalog.FindCharacterByIdentifier(identifier))
    return *character;
  throw std::runtime_error(
      "No character module is installed for module identifier " + identifier);
}

const CharacterModule& GetInputCharacterModule(const Catalog& catalog,
                                               const ThespeonInput& input) {
  const auto selection = SelectedCharacter(input);
  if (selection.module_identifier.empty())
    return catalog.SelectCharacter(selection.character_name,
                                   selection.module_type,
                                   selection.module_version);
  if (!selection.ignored_fields.empty()) {
    std::cerr << "Warning: moduleIdentifier already names one module, so "
              << JoinFields(selection.ignored_fields)
              << (selection.ignored_fields.size() == 1 ? " is" : " are")
              << " ignored\n";
  }
  return CharacterByIdentifier(catalog, selection.module_identifier);
}

std::unordered_map<std::string, LanguageModule> GetInputLanguageModules(
    const Catalog& catalog, const CharacterModule& character,
    const std::vector<std::string>& languages) {
  std::unordered_map<std::string, LanguageModule> result;
  for (const auto& requested : languages) {
    if (result.count(requested)) continue;
    std::string language;
    result.emplace(requested, catalog.CompatibleLanguageModule(
                                  character, language, requested));
  }
  return result;
}

std::mutex options_mutex;
EngineOptions engine_options;
std::size_t live_engines = 0;

// Declared first in Engine::Impl, so the count drops only after its runtime,
// and with it the thread pool, is gone.
struct Registration {
  Registration() {
    std::lock_guard lock(options_mutex);
    ++live_engines;
    options = engine_options;
  }
  ~Registration() {
    std::lock_guard lock(options_mutex);
    --live_engines;
  }
  EngineOptions options;
};

}  // namespace

ThespeonInput MakeInput(const Catalog& catalog, const std::string& model,
                        const std::string& text,
                        const std::string& module_type,
                        const Version& version,
                        const std::optional<std::string>& emotion) {
  const auto& character = catalog.SelectCharacter(model, module_type, version);
  SynthInputV100 result = PinnedTo(character);
  result.default_language.iso639_2 = "eng";
  auto& segment = result.segments.emplace_back();
  segment.text = text;
  segment.emotion = emotion;
  return result;
}

CharacterModule ValidateInput(const Catalog& catalog,
                              const ThespeonInput& input) {
  const auto& character = GetInputCharacterModule(catalog, input);
  GetInputLanguageModules(catalog, character, DistinctLanguages(input));
  ValidatePlanInput(character, input);
  return character;
}

class Engine::Impl {
 public:
  Impl(fs::path data_directory)
      : data_directory_(std::move(data_directory)),
        binaries_(data_directory_ / "binaries"),
        runtime_(binaries_, {registration_.options.intra_op_threads,
                             registration_.options.allow_spinning,
                             registration_.options.low_memory}),
        catalog_(Catalog::Load(data_directory_)) {}

  void Refresh() {
    std::unique_lock lock(mutex_);
    catalog_ = Catalog::Load(data_directory_);
    runtime_.ClearGraphs();
    ForgetSessions();
  }

  Catalog catalog() const {
    std::shared_lock lock(mutex_);
    return catalog_;
  }

  std::uintmax_t ResidentBytes() const { return runtime_.ResidentBytes(); }

  void Preload(const ThespeonInput& input, bool warmup,
               const metagraph::PreloadProgress& on_progress,
               const metagraph::CancelToken& cancel) {
    Preload(SelectCharacter(input), DistinctLanguages(input), warmup,
            on_progress, cancel);
  }

  void Preload(const std::string& module_identifier, bool warmup,
               const metagraph::PreloadProgress& on_progress,
               const metagraph::CancelToken& cancel) {
    Preload(SelectCharacter(module_identifier), {kDefaultLanguage}, warmup,
            on_progress, cancel);
  }

  void Unload(const ThespeonInput& input) {
    Unload(SelectCharacter(input), DistinctLanguages(input));
  }

  void Unload(const std::string& module_identifier) {
    Unload(SelectCharacter(module_identifier), {kDefaultLanguage});
  }

  bool IsLoaded(const ThespeonInput& input) const {
    const auto catalog_reloads = CatalogReloads();
    const auto character = SelectCharacter(input);
    if (!IsLoaded(character, catalog_reloads)) return false;
    for (const auto& [language, module] :
         LanguageModules(character, DistinctLanguages(input))) {
      if (!IsLoaded(module, catalog_reloads)) return false;
    }
    return true;
  }

  bool IsLoaded(const std::string& module_identifier) const {
    const auto catalog_reloads = CatalogReloads();
    const auto snapshot = catalog();
    if (const auto* character =
            snapshot.FindCharacterByIdentifier(module_identifier))
      return IsLoaded(*character, catalog_reloads);
    if (const auto* language =
            snapshot.FindLanguageByIdentifier(module_identifier))
      return IsLoaded(*language, catalog_reloads);
    throw std::runtime_error("No module is installed for module identifier " +
                             module_identifier);
  }

  LoadedModules GetAllLoaded() const {
    const auto catalog_reloads = CatalogReloads();
    const auto snapshot = catalog();
    // A module whose files cannot be read is left out rather than failing the
    // whole listing.
    const auto loaded = [&](const auto& module) {
      try {
        return IsLoaded(module, catalog_reloads);
      } catch (const std::exception&) {
        return false;
      }
    };
    LoadedModules result;
    for (const auto& character : snapshot.character_modules()) {
      if (loaded(character)) result.character_modules.push_back(character);
    }
    for (const auto& language : snapshot.language_modules()) {
      if (loaded(language)) result.language_modules.push_back(language);
    }
    return result;
  }

  RemoveResult Remove(const std::string& selector,
                      const std::string& module_type, const Version& version,
                      bool force, bool dry_run) {
    std::unique_lock lock(mutex_);
    // Windows will not delete a file ONNX Runtime still holds open, and which
    // sessions belong to the target is not known until it is resolved, so drop
    // them all rather than resolve it twice. Deletion is rare enough to pay
    // the reload.
    if (!dry_run) DropCaches();
    auto result = RemoveModel(selector, data_directory_, module_type, version,
                              force, dry_run);
    if (!dry_run) {
      catalog_ = Catalog::Load(data_directory_);
      ForgetSessions();
    }
    return result;
  }

  RemoveResult RemoveAll(bool dry_run) {
    std::unique_lock lock(mutex_);
    if (!dry_run) DropCaches();
    auto result = RemoveAllModels(data_directory_, dry_run);
    if (!dry_run) {
      catalog_ = Catalog::Load(data_directory_);
      ForgetSessions();
    }
    return result;
  }

  metagraph::Tensor Synthesize(const ThespeonInput& input,
                               const metagraph::CancelToken& cancel) {
    return runtime_.Execute(Prepare(SelectCharacter(input), input, cancel),
                            cancel);
  }

  void SynthesizeWithCallbacks(
      const ThespeonInput& input, metagraph::CallbackHandler handler,
      const metagraph::CancelToken& cancel) {
    THESPEON_PROFILE_SCOPE("SynthesizeWithCallbacks");
    runtime_.ExecuteWithCallbacks(
        Prepare(SelectCharacter(input), input, cancel),
        PrebufferAudio(std::move(handler)), cancel);
  }

 private:
  void Preload(const CharacterModule& character,
               const std::vector<std::string>& languages, bool warmup,
               const metagraph::PreloadProgress& on_progress,
               const metagraph::CancelToken& cancel) {
    THESPEON_PROFILE_SCOPE("Preload");
    for (const auto& graph : GraphPaths(character, languages))
      runtime_.Preload(graph, on_progress, cancel);
    if (!warmup) return;
    for (const auto& language : languages) {
      runtime_.ExecuteWithCallbacks(
          Prepare(character, WarmupInput(character, language), cancel),
          [](metagraph::CallbackPacket) {}, cancel);
    }
  }

  void Unload(const CharacterModule& character,
              const std::vector<std::string>& languages) {
    for (const auto& graph : GraphPaths(character, languages))
      runtime_.Unload(graph);
  }

  fs::path MetagraphPath(const CharacterModule& character) const {
    return LoadCharacterConfig(character).MetagraphPath(binaries_);
  }

  fs::path MetagraphPath(const LanguageModule& language) const {
    return LoadLanguageConfig(language).MetagraphPath(binaries_);
  }

  // catalog_reloads is read before the module is looked up, so a catalog
  // re-read in between keeps its sessions out of the cache.
  template <typename Module>
  bool IsLoaded(const Module& module, std::uint64_t catalog_reloads) const {
    const auto key = module.config_path.string();
    {
      std::lock_guard lock(module_sessions_mutex_);
      if (const auto found = module_sessions_.find(key);
          found != module_sessions_.end())
        return runtime_.IsResident(found->second);
    }
    auto sessions = runtime_.Sessions(MetagraphPath(module));
    const bool resident = runtime_.IsResident(sessions);
    std::lock_guard lock(module_sessions_mutex_);
    if (catalog_reloads == catalog_reloads_)
      module_sessions_.emplace(key, std::move(sessions));
    return resident;
  }

  std::uint64_t CatalogReloads() const {
    std::lock_guard lock(module_sessions_mutex_);
    return catalog_reloads_;
  }

  void ForgetSessions() {
    std::lock_guard lock(module_sessions_mutex_);
    ++catalog_reloads_;
    module_sessions_.clear();
  }

  void DropCaches() { runtime_.Clear(); }

  CharacterModule SelectCharacter(const ThespeonInput& input) const {
    std::shared_lock lock(mutex_);
    return GetInputCharacterModule(catalog_, input);
  }

  CharacterModule SelectCharacter(const std::string& module_identifier) const {
    std::shared_lock lock(mutex_);
    return CharacterByIdentifier(catalog_, module_identifier);
  }

  std::unordered_map<std::string, LanguageModule> LanguageModules(
      const CharacterModule& character,
      const std::vector<std::string>& languages) const {
    std::shared_lock lock(mutex_);
    return GetInputLanguageModules(catalog_, character, languages);
  }

  std::vector<fs::path> GraphPaths(
      const CharacterModule& character,
      const std::vector<std::string>& languages) const {
    const auto modules = LanguageModules(character, languages);
    std::vector<fs::path> result{MetagraphPath(character)};
    for (const auto& language : languages)
      result.push_back(MetagraphPath(modules.at(language)));
    return result;
  }

  static ThespeonInput WarmupInput(const CharacterModule& character,
                                   const std::string& language) {
    SynthInputV100 result = PinnedTo(character);
    result.default_language.iso639_2 = language;
    result.segments.emplace_back().text = kWarmupText;
    return result;
  }

  InferenceRequest Prepare(const CharacterModule& character,
                           ThespeonInput input,
                           const metagraph::CancelToken& cancel = {}) {
    THESPEON_PROFILE_SCOPE("Prepare");
    const auto language_modules =
        LanguageModules(character, SegmentLanguages(input));
    InferencePlan plan(character, language_modules, binaries_,
                       std::move(input));

    std::vector<metagraph::Tensor> results;
    for (auto& request : plan.PrepareLanguageModules())
      results.push_back(runtime_.Execute(std::move(request), cancel));
    plan.ApplyLanguageModuleResults(results);

    std::vector<std::string> warnings;
    auto request = plan.PrepareCharacterModule(&warnings);
    for (const auto& warning : warnings)
      std::cerr << "Warning: " << warning << '\n';
    return request;
  }

  Registration registration_;
  fs::path data_directory_;
  fs::path binaries_;
  InferenceRuntime runtime_;
  mutable std::shared_mutex mutex_;
  Catalog catalog_;
  // The sessions each module needs, keyed by config path, so a residency check
  // parses a module's config and graph once per catalog read.
  mutable std::mutex module_sessions_mutex_;
  mutable std::unordered_map<std::string, std::vector<SessionKey>>
      module_sessions_;
  std::uint64_t catalog_reloads_ = 0;
};

void SetEngineOptions(const EngineOptions& options) {
  std::lock_guard lock(options_mutex);
  if (live_engines > 0)
    throw std::runtime_error(
        "Engine options cannot change while an engine exists");
  engine_options = options;
}

Engine::Engine(fs::path data_directory)
    : impl_(std::make_unique<Impl>(std::move(data_directory))) {}
Engine::~Engine() = default;

void Engine::Preload(const ThespeonInput& input, bool warmup,
                     const metagraph::PreloadProgress& on_progress,
                     const metagraph::CancelToken& cancel) {
  impl_->Preload(input, warmup, on_progress, cancel);
}

void Engine::Preload(const std::string& module_identifier, bool warmup,
                     const metagraph::PreloadProgress& on_progress,
                     const metagraph::CancelToken& cancel) {
  impl_->Preload(module_identifier, warmup, on_progress, cancel);
}

void Engine::Unload(const ThespeonInput& input) { impl_->Unload(input); }

void Engine::Unload(const std::string& module_identifier) {
  impl_->Unload(module_identifier);
}

bool Engine::IsLoaded(const ThespeonInput& input) const {
  return impl_->IsLoaded(input);
}

bool Engine::IsLoaded(const std::string& module_identifier) const {
  return impl_->IsLoaded(module_identifier);
}

LoadedModules Engine::GetAllLoaded() const {
  return impl_->GetAllLoaded();
}

RemoveResult Engine::Remove(const std::string& selector,
                            const std::string& module_type,
                            const Version& version, bool force, bool dry_run) {
  return impl_->Remove(selector, module_type, version, force, dry_run);
}

RemoveResult Engine::RemoveAll(bool dry_run) {
  return impl_->RemoveAll(dry_run);
}

void Engine::Refresh() { impl_->Refresh(); }
Catalog Engine::catalog() const { return impl_->catalog(); }
std::uintmax_t Engine::ResidentBytes() const { return impl_->ResidentBytes(); }

void Engine::Synthesize(const ThespeonInput& input, const fs::path& output,
                        const metagraph::CancelToken& cancel) {
  WriteWav(output, impl_->Synthesize(input, cancel));
}

void Engine::SynthesizeWithCallbacks(
    const ThespeonInput& input, metagraph::CallbackHandler handler,
    const metagraph::CancelToken& cancel) {
  impl_->SynthesizeWithCallbacks(input, std::move(handler), cancel);
}

}  // namespace thespeon

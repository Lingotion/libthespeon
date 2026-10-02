// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/input/thespeon_input.h"
#include "core/module/catalog.h"
#include "core/package/remover.h"
#include "metagraph_runner.h"

namespace thespeon {

std::filesystem::path DefaultDataDirectory();

// Builds a one-segment document for a character named the way the catalog
// lists it, since the calls below name characters only by UUID.
ThespeonInput MakeInput(const Catalog& catalog, const std::string& model,
                        const std::string& text,
                        const std::string& module_type = {},
                        const Version& version = {},
                        const std::optional<std::string>& emotion = {});

// Throws what synthesis would reject the input for, short of running a graph,
// so words missing from a lookup table go unnoticed. Returns the character the
// input resolves to.
CharacterModule ValidateInput(const Catalog& catalog,
                              const ThespeonInput& input);

struct LoadedModules {
  std::vector<CharacterModule> character_modules;
  std::vector<LanguageModule> language_modules;
};

struct EngineOptions {
  int intra_op_threads = 0;
  bool allow_spinning = false;
  bool low_memory = false;
};

// Applies to every Engine created afterwards; throws while one exists.
void SetEngineOptions(const EngineOptions& options);

// Keeps loaded ONNX sessions and parsed graphs alive across synthesis calls.
// Safe to use from several threads at once, and creates none of its own.
class Engine {
 public:
  explicit Engine(std::filesystem::path data_directory);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Blocks until every model the character needs is loaded, so callers place
  // it on whichever thread they want to pay the cost on. Synthesis works
  // without it, only slower.
  //
  // Creating a session does not pay the cost of its first Run, which grows the
  // allocation arena and builds kernels for the shapes in play. warmup runs a
  // throwaway synthesis to absorb that too, and costs roughly one utterance.
  void Preload(const ThespeonInput& input, bool warmup = false,
               const metagraph::PreloadProgress& on_progress = {},
               const metagraph::CancelToken& cancel = {});
  void Preload(const std::string& module_identifier, bool warmup = false,
               const metagraph::PreloadProgress& on_progress = {},
               const metagraph::CancelToken& cancel = {});
  void Unload(const ThespeonInput& input);
  void Unload(const std::string& module_identifier);
  // An input is loaded when Preload of it would load nothing more. An
  // identifier names a character or a language module and checks only that
  // module, so a character is loaded whatever its languages. These load no
  // sessions, but the first check of a module after the catalog is read parses
  // its config and graph. GetAllLoaded leaves out modules it cannot read.
  bool IsLoaded(const ThespeonInput& input) const;
  bool IsLoaded(const std::string& module_identifier) const;
  LoadedModules GetAllLoaded() const;
  // Picks up modules imported since the catalog was last read.
  void Refresh();

  // Deletes one module and every binary no remaining module needs. A dry run
  // reports what it would do and changes nothing. Both drop every loaded
  // session, since a deleted file must not stay open. Files another Engine in
  // this process holds open are outside that guarantee.
  RemoveResult Remove(const std::string& selector,
                      const std::string& module_type = {},
                      const Version& version = {}, bool force = false,
                      bool dry_run = false);
  RemoveResult RemoveAll(bool dry_run = false);

  Catalog catalog() const;
  std::uintmax_t ResidentBytes() const;

  // A cancelled call throws metagraph::Cancelled and writes no output.
  void Synthesize(const ThespeonInput& input,
                  const std::filesystem::path& output,
                  const metagraph::CancelToken& cancel = {});
  void SynthesizeWithCallbacks(
      const ThespeonInput& input, metagraph::CallbackHandler handler,
      const metagraph::CancelToken& cancel = {});

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace thespeon

// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "audio_player.h"
#include "bench.h"
#include "core/engine.h"
#include "core/input/thespeon_input.h"
#include "core/module/catalog.h"
#include "core/package/importer.h"

namespace {

// The handler only sets a flag; a watcher thread trips the token, which takes
// a lock a signal handler must not.
volatile std::sig_atomic_t interrupted = 0;

extern "C" void OnInterrupt(int) { interrupted = 1; }

const metagraph::CancelToken& InterruptToken() {
  static const metagraph::CancelToken token =
      std::make_shared<metagraph::CancelFlag>();
  return token;
}

void WatchForInterrupt() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::signal(SIGINT, OnInterrupt);
    std::thread([] {
      while (interrupted == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      InterruptToken()->Cancel();
    }).detach();
  });
}

int Usage() {
  std::cerr << "usage:\n"
            << "  thespeon [--data-dir DIR] import <pack.lingotion>...\n"
            << "  thespeon [--data-dir DIR] models [--json]\n"
            << "  thespeon [--data-dir DIR] emotions --model MODEL "
               "[--verbose | -v] [--json]\n"
            << "  thespeon [--data-dir DIR] delete --model MODEL "
               "[--force] [--dry-run]\n"
            << "  thespeon [--data-dir DIR] delete --all [--force] "
               "[--dry-run]\n"
            << "  thespeon [--data-dir DIR] synthesize <input> "
               "[--output out.wav | --play]\n"
            << "  thespeon [--data-dir DIR] validate <input>\n"
            << "  thespeon [--data-dir DIR] bench <input> [--repeats N]\n"
            << "  thespeon [--data-dir DIR] bench <input> --sweep "
               "[--repeats N]\n"
            << "synthesize and bench also take\n"
            << "  --threads N   inference threads; 0 is the default\n"
            << "  --spinning    idle threads spin: faster, more CPU\n"
            << "  --low-memory  lower peak memory, somewhat slower\n"
            << "\n"
            << "<input> is one of:\n"
            << "  --model MODEL --text TEXT [--emotion EMOTION]\n"
            << "  --input JSON        a synthesis-input document\n"
            << "  --input-file FILE   the same document read from a file, or "
               "- for stdin\n"
            << "\n"
            << "MODEL is a character name or a module identifier, as models "
               "lists them\n"
            << "--model, here and in emotions, may be narrowed to one installed "
               "module with\n"
            << "  --module-type SIZE  one of XS, S, M, L, XL\n"
            << "  --module-version V  such as 3.0.1\n";
  return 2;
}

std::string TakeOption(const std::vector<std::string>& args,
                       const std::string& name) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == name && i + 1 < args.size()) return args[i + 1];
    const auto prefix = name + "=";
    if (args[i].rfind(prefix, 0) == 0) return args[i].substr(prefix.size());
  }
  return {};
}

bool HasFlag(const std::vector<std::string>& args, const std::string& name) {
  return std::find(args.begin(), args.end(), name) != args.end();
}

bool HasOption(const std::vector<std::string>& args, const std::string& name) {
  return std::any_of(args.begin(), args.end(), [&](const std::string& arg) {
    return arg == name || arg.rfind(name + "=", 0) == 0;
  });
}

// A mistake on the command line, reported with the usage text.
class UsageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

const std::vector<std::string> kInputOptions = {
    "--model",       "--text",           "--input",  "--input-file",
    "--module-type", "--module-version", "--emotion"};

// Read by SetInferenceOptions; synthesize and bench both take them.
const std::vector<std::string> kInferenceOptions = {"--threads"};
const std::vector<std::string> kInferenceFlags = {"--spinning", "--low-memory"};

// A value that looks like an option is almost always a missing value, as in
// "--model --text hi". One that really starts with "--" can use "name=value".
void CheckValue(const std::string& name, const std::vector<std::string>& args,
                std::size_t index) {
  if (index >= args.size()) throw UsageError(name + " needs a value");
  if (args[index].rfind("--", 0) == 0) {
    throw UsageError(name + " needs a value, but is followed by " +
                     args[index] + "; use " + name + "=VALUE if the value " +
                     "starts with --");
  }
}

// Rejects anything that is not one of the given options or flags, so a typo
// fails instead of being silently ignored.
void CheckOptions(const std::vector<std::string>& args,
                  const std::vector<std::string>& options,
                  const std::vector<std::string>& flags) {
  std::vector<std::string> seen;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto& arg = args[i];
    const auto name = arg.substr(0, arg.find('='));
    const bool is_option =
        std::find(options.begin(), options.end(), name) != options.end();
    const bool is_flag = arg == name && std::find(flags.begin(), flags.end(),
                                                  name) != flags.end();
    if (!is_option && !is_flag) {
      throw UsageError(arg.rfind("--", 0) == 0 ? "Unknown option " + arg
                                               : "Unexpected argument " + arg);
    }
    if (std::find(seen.begin(), seen.end(), name) != seen.end())
      throw UsageError(name + " was given more than once");
    seen.push_back(name);
    if (is_option && arg == name) CheckValue(name, args, ++i);
  }
}

std::vector<std::string> Concat(std::vector<std::string> first,
                                const std::vector<std::string>& second) {
  first.insert(first.end(), second.begin(), second.end());
  return first;
}

std::string ReadAll(const std::string& path) {
  if (path == "-")
    return {std::istreambuf_iterator<char>(std::cin),
            std::istreambuf_iterator<char>()};
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Could not open " + path);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

// Nullopt means no usable input was given, which is a usage error.
std::optional<thespeon::ThespeonInput> TakeInput(
    const std::filesystem::path& data_directory,
    const std::vector<std::string>& args) {
  const auto json = TakeOption(args, "--input");
  const auto path = TakeOption(args, "--input-file");
  const auto model = TakeOption(args, "--model");
  const auto text = TakeOption(args, "--text");
  const auto module_type = TakeOption(args, "--module-type");
  const auto module_version = TakeOption(args, "--module-version");
  const auto emotion = TakeOption(args, "--emotion");
  for (const auto& name : kInputOptions) {
    if (HasOption(args, name) && TakeOption(args, name).empty())
      throw UsageError(name + " is empty");
  }
  if (!json.empty() && !path.empty())
    throw UsageError("Pass either --input or --input-file, not both");
  if ((!json.empty() || !path.empty()) && (!model.empty() || !text.empty()))
    throw UsageError("Pass either a document or --model and --text, not both");
  if ((!json.empty() || !path.empty()) &&
      (!module_type.empty() || !module_version.empty()))
    throw UsageError(
        "--module-type and --module-version narrow --model; a document names "
        "its module itself");
  if ((!json.empty() || !path.empty()) && !emotion.empty())
    throw UsageError(
        "--emotion goes with --model and --text; a document names its "
        "emotions itself");
  if (!json.empty()) return thespeon::ParseThespeonInput(json);
  if (!path.empty()) return thespeon::ParseThespeonInput(ReadAll(path));
  if (model.empty() && text.empty()) {
    if (!module_type.empty() || !module_version.empty())
      throw UsageError("--module-type and --module-version narrow --model");
    if (!emotion.empty())
      throw UsageError("--emotion goes with --model and --text");
    return std::nullopt;
  }
  if (text.empty()) throw UsageError("--model needs --text");
  if (model.empty()) throw UsageError("--text needs --model");

  const auto version = thespeon::ParseNarrowing(module_type, module_version);
  return thespeon::MakeInput(
      thespeon::Catalog::Load(data_directory), model, text, module_type,
      version,
      emotion.empty() ? std::nullopt : std::optional<std::string>(emotion));
}

void SetInferenceOptions(const std::vector<std::string>& args) {
  thespeon::EngineOptions config;
  if (const auto threads = TakeOption(args, "--threads"); !threads.empty()) {
    std::size_t used = 0;
    try {
      config.intra_op_threads = std::stoi(threads, &used);
    } catch (const std::exception&) {
    }
    if (config.intra_op_threads < 0 || used != threads.size())
      throw std::runtime_error("--threads takes a non-negative number, not " +
                               threads);
  }
  config.allow_spinning = HasFlag(args, "--spinning");
  config.low_memory = HasFlag(args, "--low-memory");
  thespeon::SetEngineOptions(config);
}

std::string HumanBytes(std::uintmax_t bytes) {
  static const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' '
         << units[unit];
  return output.str();
}

bool IsInteractive() {
#ifdef _WIN32
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(STDIN_FILENO) != 0;
#endif
}

void PrintReport(const thespeon::RemoveResult& result,
                 const std::filesystem::path& data_directory) {
  std::cout << "Would delete " << result.target << " from "
            << data_directory.string() << '\n';
  if (result.binaries_removed > 0) {
    std::cout << "  " << result.binaries_removed
              << " file(s) would be removed, " << HumanBytes(result.bytes_freed)
              << " freed\n";
  }
  if (result.binaries_kept > 0) {
    std::cout << "  " << result.binaries_kept
              << " file(s) kept, still needed by another module\n";
  }
  if (!result.stranded_characters.empty()) {
    std::cout << "  " << result.stranded_characters.size()
              << " character(s) would be left with no language module:";
    for (const auto& character : result.stranded_characters)
      std::cout << " \"" << character << '"';
    std::cout << "\n      pass --force to delete anyway\n";
  }
  if (!result.swept) {
    std::cout << "  Shared files could not be swept, because:\n";
    for (const auto& problem : result.problems)
      std::cout << "    " << problem << '\n';
  }
}

void PrintOutcome(const thespeon::RemoveResult& result) {
  std::cout << "Deleted " << result.target << ": " << result.binaries_removed
            << " file(s) removed, " << HumanBytes(result.bytes_freed)
            << " freed.\n";
  if (result.swept) return;
  for (const auto& problem : result.problems) {
    std::cerr << "Warning: " << problem
              << ", so shared files were not swept.\n";
  }
}

bool Confirm(bool all) {
  std::cout << (all ? "Delete them? [y/N] " : "Delete it? [y/N] ")
            << std::flush;
  std::string answer;
  std::getline(std::cin, answer);
  std::transform(answer.begin(), answer.end(), answer.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return answer == "y" || answer == "yes";
}

int DeleteModel(const std::filesystem::path& data_directory,
                const std::vector<std::string>& args) {
  CheckOptions(args, {"--model", "--module-type", "--module-version"},
               {"--all", "--force", "--dry-run"});
  const auto model = TakeOption(args, "--model");
  const bool all = HasFlag(args, "--all");
  const bool force = HasFlag(args, "--force");
  const bool dry_run = HasFlag(args, "--dry-run");
  if (all == !model.empty()) return Usage();
  if (force && dry_run) return Usage();

  const auto module_type = TakeOption(args, "--module-type");
  const auto version = thespeon::ParseNarrowing(
      module_type, TakeOption(args, "--module-version"));
  if (all && (!module_type.empty() || version.IsValid())) return Usage();

  thespeon::Engine engine(data_directory);
  const auto run = [&](bool preview) {
    return all ? engine.RemoveAll(preview)
               : engine.Remove(model, module_type, version, force, preview);
  };

  if (!force) {
    PrintReport(run(true), data_directory);
    if (dry_run) return 0;
    if (!IsInteractive())
      throw std::runtime_error(
          "--force is required when stdin is not a terminal");
    if (!Confirm(all)) {
      std::cout << "Cancelled.\n";
      return 0;
    }
  }
  PrintOutcome(run(false));
  return 0;
}

// Dashes stand in for a pack built before module_identifier existed. They
// keep the identifier column 32 characters wide so the rows stay aligned,
// and such a character module can still be deleted by its name.
std::string Identifier(const thespeon::Module& module) {
  return module.identifier.empty() ? std::string(32, '-') : module.identifier;
}

// Exits 1 when the data directory is missing or any module failed to load, so
// scripts notice a mistyped --data-dir or a partial listing.
int ListModels(const std::filesystem::path& data_directory, bool json) {
  if (!std::filesystem::exists(data_directory))
    throw std::runtime_error(
        "The data directory " + data_directory.string() +
        " does not exist; import a pack with thespeon import "
        "<pack.lingotion>, or pass --data-dir to read another directory");
  const auto catalog = thespeon::Catalog::Load(data_directory);
  const int status = catalog.problems().empty() ? 0 : 1;
  if (json) {
    std::cout << thespeon::CatalogJson(catalog) << '\n';
    return status;
  }
  std::cout << "Data directory: " << data_directory.string() << '\n';
  std::cout << "Character modules (" << catalog.character_modules().size()
            << "):\n";
  for (const auto& module : catalog.character_modules()) {
    std::cout << "  " << Identifier(module) << "  \"" << module.character_name
              << "\"  " << thespeon::ModuleTypeName(module.quality)
              << "  languages:";
    for (const auto& language : module.languages)
      std::cout << ' ' << language.Display();
    std::cout << "  " << module.version.ToString() << '\n';
  }
  std::cout << "Language modules (" << catalog.language_modules().size()
            << "):\n";
  for (const auto& module : catalog.language_modules()) {
    std::cout << "  " << Identifier(module) << "  languages:";
    for (const auto& language : module.languages)
      std::cout << ' ' << language.DisplayFull();
    std::cout << "  " << module.version.ToString() << '\n';
  }
  for (const auto& problem : catalog.problems())
    std::cerr << "Warning: " << problem << '\n';
  if (catalog.character_modules().empty() &&
      catalog.language_modules().empty() && catalog.problems().empty()) {
    std::cerr << "No modules installed; import a pack with thespeon import "
                 "<pack.lingotion>, or pass --data-dir to read another "
                 "directory.\n";
  }
  return status;
}

std::string Describe(const thespeon::CharacterModule& module) {
  return Identifier(module) + "  \"" + module.character_name + "\"  " +
         thespeon::ModuleTypeName(module.quality) + "  " +
         module.version.ToString();
}

int ListEmotions(const std::filesystem::path& data_directory,
                 const std::vector<std::string>& args) {
  CheckOptions(args, {"--model", "--module-type", "--module-version"},
               {"--json", "--verbose", "-v"});
  const auto model = TakeOption(args, "--model");
  if (model.empty()) throw UsageError("emotions needs --model");
  const auto module_type = TakeOption(args, "--module-type");
  const auto catalog = thespeon::Catalog::Load(data_directory);
  const auto& character = catalog.SelectCharacter(
      model, module_type,
      thespeon::ParseNarrowing(module_type,
                               TakeOption(args, "--module-version")));
  if (HasFlag(args, "--json")) {
    std::cout << thespeon::EmotionsJson(character) << '\n';
    return 0;
  }
  const bool verbose = HasFlag(args, "--verbose") || HasFlag(args, "-v");
  std::cout << Describe(character) << '\n';
  std::cout << "Emotions (" << character.emotions.size() << "):\n";
  for (const auto& emotion : character.emotions) {
    std::cout << "  " << emotion.name << '\n';
    if (verbose && !emotion.guide.empty())
      std::cout << "      " << emotion.guide << '\n';
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    std::vector<std::string> args;
    std::filesystem::path data_directory;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      const auto& argument = arguments[i];
      if (argument == "--data-dir") {
        CheckValue(argument, arguments, ++i);
        data_directory = arguments[i];
      } else if (argument.rfind("--data-dir=", 0) == 0) {
        data_directory = argument.substr(11);
      } else {
        args.push_back(argument);
      }
    }
    if (data_directory.empty()) data_directory = thespeon::DefaultDataDirectory();
    if (args.empty() || args[0] == "--help" || args[0] == "-h") return Usage();

    if (args[0] == "import") {
      if (args.size() < 2) return Usage();
      for (std::size_t i = 1; i < args.size(); ++i) {
        const auto result = thespeon::ImportPack(args[i], data_directory);
        std::cout << "Imported " << args[i] << ": " << result.written
                  << " file(s) written, " << result.unchanged
                  << " unchanged.\n";
      }
      return 0;
    }
    if (args[0] == "delete") {
      return DeleteModel(data_directory,
                         std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "models") {
      const std::vector<std::string> options(args.begin() + 1, args.end());
      CheckOptions(options, {}, {"--json"});
      return ListModels(data_directory, HasFlag(options, "--json"));
    }
    if (args[0] == "emotions") {
      return ListEmotions(data_directory,
                          std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "validate") {
      const std::vector<std::string> options(args.begin() + 1, args.end());
      CheckOptions(options, kInputOptions, {});
      const auto input = TakeInput(data_directory, options);
      if (!input) return Usage();
      const auto character = thespeon::ValidateInput(
          thespeon::Catalog::Load(data_directory), *input);
      std::cout << "OK: " << Describe(character) << '\n';
      return 0;
    }
    if (args[0] == "synthesize") {
      const std::vector<std::string> options(args.begin() + 1, args.end());
      CheckOptions(options,
                   Concat(Concat(kInputOptions, kInferenceOptions), {"--output"}),
                   Concat(kInferenceFlags, {"--play"}));
      const auto input = TakeInput(data_directory, options);
      auto output = TakeOption(options, "--output");
      const bool play = HasFlag(options, "--play");
      if (!input) return Usage();
      if (play && HasOption(options, "--output")) return Usage();
      SetInferenceOptions(options);
      WatchForInterrupt();
      if (play) {
        thespeon::cli::PlaySynthesis(data_directory, *input, InterruptToken());
        std::cout << "Playback complete\n";
        return 0;
      }
      if (output.empty()) output = "out.wav";
      thespeon::Engine(data_directory)
          .Synthesize(*input, output, InterruptToken());
      std::cout << "Wrote " << output << '\n';
      return 0;
    }
    if (args[0] == "bench") {
      const std::vector<std::string> options(args.begin() + 1, args.end());
      CheckOptions(options,
                   Concat(Concat(kInputOptions, kInferenceOptions), {"--repeats"}),
                   Concat(kInferenceFlags, {"--sweep"}));
      const auto input = TakeInput(data_directory, options);
      const auto repeats = TakeOption(options, "--repeats");
      if (!input) return Usage();
      if (HasFlag(options, "--sweep")) {
        if (HasOption(options, "--threads") || HasFlag(options, "--spinning") ||
            HasFlag(options, "--low-memory"))
          return Usage();
        thespeon::cli::RunSweep(data_directory, *input,
                                repeats.empty() ? 3 : std::stoi(repeats));
        return 0;
      }
      SetInferenceOptions(options);
      thespeon::cli::RunBenchmark(data_directory, *input,
                                  repeats.empty() ? 2 : std::stoi(repeats));
      return 0;
    }
    return Usage();
  } catch (const metagraph::Cancelled&) {
    std::cerr << "Cancelled\n";
    return 130;
  } catch (const UsageError& error) {
    std::cerr << "Error: " << error.what() << "\n\n";
    return Usage();
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}

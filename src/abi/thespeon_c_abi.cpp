// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "thespeon.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/engine.h"
#include "core/input/input_builder.h"
#include "core/input/thespeon_input.h"
#include "core/module/catalog.h"
#include "core/package/importer.h"

namespace {

namespace fs = std::filesystem;

thread_local std::string last_error;

class InvalidArgument : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void SetLastError(const char* message) noexcept {
  try {
    last_error = message;
  } catch (...) {
    last_error.clear();
  }
}

template <typename Function>
thespeon_status Invoke(Function&& function) noexcept {
  last_error.clear();
  try {
    std::forward<Function>(function)();
    return THESPEON_STATUS_OK;
  } catch (const metagraph::Cancelled& error) {
    SetLastError(error.what());
    return THESPEON_STATUS_CANCELLED;
  } catch (const InvalidArgument& error) {
    SetLastError(error.what());
    return THESPEON_STATUS_INVALID_ARGUMENT;
  } catch (const metagraph::GraphError& error) {
    SetLastError(error.what());
    return THESPEON_STATUS_GRAPH_ERROR;
  } catch (const std::invalid_argument& error) {
    SetLastError(error.what());
    return THESPEON_STATUS_INVALID_ARGUMENT;
  } catch (const std::exception& error) {
    SetLastError(error.what());
    return THESPEON_STATUS_RUNTIME_ERROR;
  } catch (...) {
    SetLastError("Unknown thespeon error");
    return THESPEON_STATUS_RUNTIME_ERROR;
  }
}

bool IsValidUtf8(std::string_view input) {
  for (std::size_t i = 0; i < input.size();) {
    const auto lead = static_cast<unsigned char>(input[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }

    std::size_t continuation_count = 0;
    char32_t code_point = 0;
    char32_t minimum = 0;
    if ((lead & 0xe0) == 0xc0) {
      continuation_count = 1;
      code_point = lead & 0x1f;
      minimum = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
      continuation_count = 2;
      code_point = lead & 0x0f;
      minimum = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
      continuation_count = 3;
      code_point = lead & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }

    if (i + continuation_count >= input.size()) return false;
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(input[i + offset]);
      if ((continuation & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (continuation & 0x3f);
    }
    if (code_point < minimum || code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
      return false;
    }
    i += continuation_count + 1;
  }
  return true;
}

std::string Utf8Argument(const char* value, const char* name) {
  if (value == nullptr) throw InvalidArgument(std::string(name) + " is null");
  std::string result(value);
  if (result.empty()) throw InvalidArgument(std::string(name) + " is empty");
  if (!IsValidUtf8(result))
    throw InvalidArgument(std::string(name) + " is not valid UTF-8");
  return result;
}

fs::path Utf8Path(const char* value, const char* name) {
  const auto string = Utf8Argument(value, name);
#if defined(__cpp_char8_t)
  return fs::path(std::u8string(
      reinterpret_cast<const char8_t*>(string.data()), string.size()));
#else
  return fs::u8path(string);
#endif
}

// Null narrowing narrows nothing, as an omitted CLI option does.
std::string OptionalArgument(const char* value, const char* name) {
  return value == nullptr ? std::string{} : Utf8Argument(value, name);
}

thespeon::Version Narrowing(const char* module_type,
                            const char* module_version) {
  return thespeon::ParseNarrowing(
      OptionalArgument(module_type, "module_type"),
      OptionalArgument(module_version, "module_version"));
}

void StoreDeleteResult(const thespeon::RemoveResult& removed,
                       thespeon_delete_result* result) {
  result->binaries_removed = static_cast<uint64_t>(removed.binaries_removed);
  result->bytes_freed = static_cast<uint64_t>(removed.bytes_freed);
  result->binaries_kept = static_cast<uint64_t>(removed.binaries_kept);
  result->stranded_characters =
      static_cast<uint64_t>(removed.stranded_characters.size());
  result->problems = static_cast<uint64_t>(removed.problems.size());
  result->swept = removed.swept ? 1 : 0;
}

void ClearDeleteResult(thespeon_delete_result* result) {
  if (result == nullptr) throw InvalidArgument("result is null");
  *result = thespeon_delete_result{};
}

char* CopyString(const std::string& value) {
  if (value.size() == std::numeric_limits<std::size_t>::max())
    throw std::length_error("Result string is too large");
  auto copy = std::make_unique<char[]>(value.size() + 1);
  std::memcpy(copy.get(), value.c_str(), value.size() + 1);
  return copy.release();
}

thespeon_version VersionView(const thespeon::Version& version) {
  return {version.major, version.minor, version.patch};
}

std::vector<thespeon_module_language> LanguageViews(
    const std::vector<thespeon::Language>& languages) {
  std::vector<thespeon_module_language> result;
  result.reserve(languages.size());
  for (const auto& language : languages) {
    result.push_back({language.iso639_2.c_str(), language.iso639_3.c_str(),
                      language.glottocode.c_str(), language.country.c_str(),
                      language.iso3166_2.c_str(),
                      language.custom_dialect.c_str(),
                      language.name_in_english.c_str(),
                      language.autonym.c_str()});
  }
  return result;
}

std::vector<thespeon_emotion_description> EmotionViews(
    const std::vector<thespeon::EmotionDescription>& emotions) {
  std::vector<thespeon_emotion_description> result;
  result.reserve(emotions.size());
  for (const auto& emotion : emotions)
    result.push_back({emotion.name.c_str(), emotion.guide.c_str()});
  return result;
}

// Owns what its views point into, and never resizes once they exist.
class CharacterModuleViews {
 public:
  explicit CharacterModuleViews(std::vector<thespeon::CharacterModule> modules)
      : modules_(std::move(modules)) {
    module_types_.reserve(modules_.size());
    languages_.reserve(modules_.size());
    emotions_.reserve(modules_.size());
    views_.reserve(modules_.size());
    for (const auto& module : modules_) {
      module_types_.push_back(thespeon::ModuleTypeName(module.quality));
      languages_.push_back(LanguageViews(module.languages));
      emotions_.push_back(EmotionViews(module.emotions));
      views_.push_back({module.identifier.c_str(),
                        module.character_name.c_str(),
                        module_types_.back().c_str(),
                        VersionView(module.version), languages_.back().data(),
                        static_cast<uint64_t>(languages_.back().size()),
                        emotions_.back().data(),
                        static_cast<uint64_t>(emotions_.back().size())});
    }
  }

  const thespeon_character_module* data() const { return views_.data(); }
  uint64_t size() const { return static_cast<uint64_t>(views_.size()); }

 private:
  std::vector<thespeon::CharacterModule> modules_;
  std::vector<std::string> module_types_;
  std::vector<std::vector<thespeon_module_language>> languages_;
  std::vector<std::vector<thespeon_emotion_description>> emotions_;
  std::vector<thespeon_character_module> views_;
};

class LanguageModuleViews {
 public:
  explicit LanguageModuleViews(std::vector<thespeon::LanguageModule> modules)
      : modules_(std::move(modules)) {
    languages_.reserve(modules_.size());
    views_.reserve(modules_.size());
    for (const auto& module : modules_) {
      languages_.push_back(LanguageViews(module.languages));
      views_.push_back({module.identifier.c_str(), module.name.c_str(),
                        VersionView(module.version), languages_.back().data(),
                        static_cast<uint64_t>(languages_.back().size())});
    }
  }

  const thespeon_language_module* data() const { return views_.data(); }
  uint64_t size() const { return static_cast<uint64_t>(views_.size()); }

 private:
  std::vector<thespeon::LanguageModule> modules_;
  std::vector<std::vector<thespeon_module_language>> languages_;
  std::vector<thespeon_language_module> views_;
};

// Derives from the struct it hands out, so freeing can cast back.
struct CatalogStorage : thespeon_catalog {
  explicit CatalogStorage(const thespeon::Catalog& catalog)
      : thespeon_catalog{},
        characters(catalog.character_modules()),
        languages(catalog.language_modules()),
        owned_problems(catalog.problems()) {
    for (const auto& problem : owned_problems)
      problem_views.push_back(problem.c_str());
    character_modules = characters.data();
    character_module_count = characters.size();
    language_modules = languages.data();
    language_module_count = languages.size();
    problems = problem_views.data();
    problem_count = static_cast<uint64_t>(problem_views.size());
  }

  CharacterModuleViews characters;
  LanguageModuleViews languages;
  std::vector<std::string> owned_problems;
  std::vector<const char*> problem_views;
};

struct ModuleListStorage : thespeon_module_list {
  explicit ModuleListStorage(thespeon::LoadedModules loaded)
      : thespeon_module_list{},
        characters(std::move(loaded.character_modules)),
        languages(std::move(loaded.language_modules)) {
    character_modules = characters.data();
    character_module_count = characters.size();
    language_modules = languages.data();
    language_module_count = languages.size();
  }

  CharacterModuleViews characters;
  LanguageModuleViews languages;
};

thespeon_catalog* NewCatalog(const thespeon::Catalog& catalog) {
  return new CatalogStorage(catalog);
}

bool IsFinalAudioPacket(const metagraph::CallbackPacket& packet) {
  const auto found = packet.metadata.find("is_final");
  if (found == packet.metadata.end()) return false;
  const auto* value = std::get_if<bool>(&found->second);
  if (value == nullptr)
    throw std::runtime_error(
        "Audio callback metadata 'is_final' is not boolean");
  return *value;
}

void StreamThrough(thespeon::Engine& engine,
                   const thespeon::ThespeonInput& input,
                   thespeon_audio_callback callback,
                   thespeon_sample_callback on_samples, void* user_data,
                   const metagraph::CancelToken& cancel) {
  if (callback == nullptr) throw InvalidArgument("callback is null");

  bool received_audio = false;
  bool received_final = false;
  engine.SynthesizeWithCallbacks(
      input, [&](metagraph::CallbackPacket packet) {
        if (packet.type == metagraph::CallbackType::TriggerSample) {
          if (on_samples == nullptr) return;
          const auto* indices =
              std::get_if<std::vector<std::int64_t>>(&packet.payload);
          if (indices == nullptr)
            throw std::runtime_error(
                "Audio sample request callback has an invalid payload");
          on_samples(indices->data(), static_cast<uint64_t>(indices->size()),
                     user_data);
          return;
        }
        if (packet.type != metagraph::CallbackType::Audio) return;
        if (received_final)
          throw std::runtime_error("Received audio after the final chunk");
        const auto* samples = std::get_if<std::vector<float>>(&packet.payload);
        if (samples == nullptr)
          throw std::runtime_error("Audio callback has an invalid payload");
        const bool is_final = IsFinalAudioPacket(packet);
        callback(samples->data(), static_cast<uint64_t>(samples->size()),
                 is_final ? UINT8_C(1) : UINT8_C(0), user_data);
        received_audio = received_audio || !samples->empty();
        received_final = received_final || is_final;
      },
      cancel);

  if (!received_audio)
    throw std::runtime_error("Character module produced no audio");
  if (!received_final)
    throw std::runtime_error(
        "Character-module audio did not include final-chunk metadata");
}

struct CancelTokenHandle {
  metagraph::CancelToken token;
};

// Copies the shared_ptr so the flag outlives a destroy racing an unwinding run.
metagraph::CancelToken TokenOf(const thespeon_cancel_token* handle) {
  if (handle == nullptr) return {};
  return reinterpret_cast<const CancelTokenHandle*>(handle)->token;
}

thespeon::Engine& Unwrap(thespeon_engine* engine) {
  if (engine == nullptr) throw InvalidArgument("engine is null");
  return *reinterpret_cast<thespeon::Engine*>(engine);
}

thespeon::InputBuilder& Unwrap(thespeon_input* input) {
  if (input == nullptr) throw InvalidArgument("input is null");
  return *reinterpret_cast<thespeon::InputBuilder*>(input);
}

const thespeon::InputBuilder& Unwrap(const thespeon_input* input) {
  if (input == nullptr) throw InvalidArgument("input is null");
  return *reinterpret_cast<const thespeon::InputBuilder*>(input);
}

std::optional<std::string> NullableArgument(const char* value,
                                            const char* name) {
  if (value == nullptr) return std::nullopt;
  return Utf8Argument(value, name);
}

thespeon::ModuleLanguage LanguageArgument(const thespeon_language& language) {
  thespeon::ModuleLanguage result;
  result.iso639_2 = Utf8Argument(language.iso639_2, "iso639_2");
  result.iso639_3 = NullableArgument(language.iso639_3, "iso639_3");
  result.glottocode = NullableArgument(language.glottocode, "glottocode");
  result.iso3166_1 = NullableArgument(language.iso3166_1, "iso3166_1");
  result.iso3166_2 = NullableArgument(language.iso3166_2, "iso3166_2");
  result.custom_dialect =
      NullableArgument(language.custom_dialect, "custom_dialect");
  return result;
}

thespeon::Endpoint EndpointArgument(thespeon_endpoint at) {
  if (at == THESPEON_ENDPOINT_START) return thespeon::Endpoint::Start;
  if (at == THESPEON_ENDPOINT_END) return thespeon::Endpoint::End;
  throw InvalidArgument("at is not a thespeon_endpoint");
}

std::size_t SegmentArgument(uint64_t segment) {
  if (segment > std::numeric_limits<std::size_t>::max())
    throw InvalidArgument("segment is out of range");
  return static_cast<std::size_t>(segment);
}

template <typename Function>
thespeon_status PreloadWith(thespeon_progress_callback progress,
                            void* user_data, Function&& preload) noexcept {
  return Invoke([&] {
    metagraph::PreloadProgress forward;
    if (progress != nullptr) {
      forward = [&](std::size_t done, std::size_t total) {
        return progress(static_cast<uint64_t>(done),
                        static_cast<uint64_t>(total), user_data) != 0;
      };
    }
    preload(forward);
  });
}

}  // namespace

extern "C" {

uint32_t thespeon_get_abi_version(void) { return THESPEON_ABI_VERSION; }

thespeon_status thespeon_cancel_token_create(thespeon_cancel_token** out_token) {
  return Invoke([&] {
    if (out_token == nullptr) throw InvalidArgument("out_token is null");
    *out_token = nullptr;
    auto handle = std::make_unique<CancelTokenHandle>();
    handle->token = std::make_shared<metagraph::CancelFlag>();
    *out_token = reinterpret_cast<thespeon_cancel_token*>(handle.release());
  });
}

void thespeon_cancel_token_cancel(thespeon_cancel_token* token) {
  if (token == nullptr) return;
  reinterpret_cast<CancelTokenHandle*>(token)->token->Cancel();
}

uint8_t thespeon_cancel_token_is_cancelled(const thespeon_cancel_token* token) {
  if (token == nullptr) return 0;
  return reinterpret_cast<const CancelTokenHandle*>(token)->token->cancelled()
             ? UINT8_C(1)
             : UINT8_C(0);
}

void thespeon_cancel_token_destroy(thespeon_cancel_token* token) {
  delete reinterpret_cast<CancelTokenHandle*>(token);
}

thespeon_status thespeon_import_pack(const char* data_directory,
                                     const char* pack_path,
                                     thespeon_import_result* result) {
  return Invoke([&] {
    if (result == nullptr) throw InvalidArgument("result is null");
    result->written = 0;
    result->unchanged = 0;
    const auto imported = thespeon::ImportPack(
        Utf8Path(pack_path, "pack_path"),
        Utf8Path(data_directory, "data_directory"));
    result->written = static_cast<uint64_t>(imported.written);
    result->unchanged = static_cast<uint64_t>(imported.unchanged);
  });
}

thespeon_status thespeon_list_models(const char* data_directory,
                                     thespeon_catalog** out_catalog) {
  return Invoke([&] {
    if (out_catalog == nullptr) throw InvalidArgument("out_catalog is null");
    *out_catalog = nullptr;
    *out_catalog = NewCatalog(
        thespeon::Engine(Utf8Path(data_directory, "data_directory")).catalog());
  });
}

thespeon_status thespeon_validate_input(const char* data_directory,
                                        const thespeon_input* input) {
  return Invoke([&] {
    thespeon::ValidateInput(
        thespeon::Engine(Utf8Path(data_directory, "data_directory")).catalog(),
        Unwrap(input).Build());
  });
}

thespeon_status thespeon_delete_preview(const char* data_directory,
                                        const char* model,
                                        const char* module_type,
                                        const char* module_version,
                                        thespeon_delete_result* result) {
  return Invoke([&] {
    ClearDeleteResult(result);
    thespeon::Engine engine(Utf8Path(data_directory, "data_directory"));
    StoreDeleteResult(
        model == nullptr
            ? engine.RemoveAll(true)
            : engine.Remove(Utf8Argument(model, "model"),
                            OptionalArgument(module_type, "module_type"),
                            Narrowing(module_type, module_version), false,
                            true),
        result);
  });
}

thespeon_status thespeon_delete_model(const char* data_directory,
                                      const char* model,
                                      const char* module_type,
                                      const char* module_version, uint8_t force,
                                      thespeon_delete_result* result) {
  return Invoke([&] {
    ClearDeleteResult(result);
    StoreDeleteResult(
        thespeon::Engine(Utf8Path(data_directory, "data_directory"))
            .Remove(Utf8Argument(model, "model"),
                    OptionalArgument(module_type, "module_type"),
                    Narrowing(module_type, module_version), force != 0, false),
        result);
  });
}

thespeon_status thespeon_delete_all_models(const char* data_directory,
                                           thespeon_delete_result* result) {
  return Invoke([&] {
    ClearDeleteResult(result);
    StoreDeleteResult(
        thespeon::Engine(Utf8Path(data_directory, "data_directory"))
            .RemoveAll(false),
        result);
  });
}

thespeon_status thespeon_input_create(thespeon_input** out_input) {
  return Invoke([&] {
    if (out_input == nullptr) throw InvalidArgument("out_input is null");
    *out_input = nullptr;
    auto input = std::make_unique<thespeon::InputBuilder>();
    *out_input = reinterpret_cast<thespeon_input*>(input.release());
  });
}

thespeon_status thespeon_input_from_json(const char* input_json,
                                        thespeon_input** out_input) {
  return Invoke([&] {
    if (out_input == nullptr) throw InvalidArgument("out_input is null");
    *out_input = nullptr;
    auto input = std::make_unique<thespeon::InputBuilder>(
        thespeon::InputBuilder::FromJson(
            Utf8Argument(input_json, "input_json")));
    *out_input = reinterpret_cast<thespeon_input*>(input.release());
  });
}

void thespeon_input_destroy(thespeon_input* input) {
  delete reinterpret_cast<thespeon::InputBuilder*>(input);
}

thespeon_status thespeon_input_to_json(const thespeon_input* input,
                                       char** result_json) {
  return Invoke([&] {
    if (result_json == nullptr) throw InvalidArgument("result_json is null");
    *result_json = nullptr;
    *result_json = CopyString(Unwrap(input).ToJson());
  });
}

thespeon_status thespeon_input_set_character_module(
    thespeon_input* input, const char* module_identifier) {
  return Invoke([&] {
    Unwrap(input).SetCharacterModule(
        Utf8Argument(module_identifier, "module_identifier"));
  });
}

thespeon_status thespeon_input_set_character(thespeon_input* input,
                                             const char* character_name,
                                             const char* module_type,
                                             const char* module_version) {
  return Invoke([&] {
    Unwrap(input).SetCharacter(
        Utf8Argument(character_name, "character_name"),
        NullableArgument(module_type, "module_type"),
        NullableArgument(module_version, "module_version"));
  });
}

thespeon_status thespeon_input_set_default_emotion(thespeon_input* input,
                                                   const char* emotion) {
  return Invoke([&] {
    Unwrap(input).SetDefaultEmotion(NullableArgument(emotion, "emotion"));
  });
}

thespeon_status thespeon_input_set_default_language(
    thespeon_input* input, const thespeon_language* language) {
  return Invoke([&] {
    if (language == nullptr) throw InvalidArgument("language is null");
    Unwrap(input).SetDefaultLanguage(LanguageArgument(*language));
  });
}

thespeon_status thespeon_input_add_segment(thespeon_input* input,
                                           const char* text,
                                           uint64_t* out_index) {
  return Invoke([&] {
    const auto index = Unwrap(input).AddSegment(Utf8Argument(text, "text"));
    if (out_index != nullptr) *out_index = static_cast<uint64_t>(index);
  });
}

thespeon_status thespeon_input_segment_set_custom_pronounced(
    thespeon_input* input, uint64_t segment, uint8_t enabled) {
  return Invoke([&] {
    Unwrap(input).SetCustomPronounced(SegmentArgument(segment), enabled != 0);
  });
}

thespeon_status thespeon_input_segment_set_language(
    thespeon_input* input, uint64_t segment,
    const thespeon_language* language) {
  return Invoke([&] {
    std::optional<thespeon::ModuleLanguage> value;
    if (language != nullptr) value = LanguageArgument(*language);
    Unwrap(input).SetSegmentLanguage(SegmentArgument(segment), value);
  });
}

thespeon_status thespeon_input_segment_set_emotion(thespeon_input* input,
                                                   uint64_t segment,
                                                   const char* emotion) {
  return Invoke([&] {
    Unwrap(input).SetSegmentEmotion(SegmentArgument(segment),
                                    NullableArgument(emotion, "emotion"));
  });
}

thespeon_status thespeon_input_segment_add_emotion(thespeon_input* input,
                                                   uint64_t segment,
                                                   thespeon_endpoint at,
                                                   const char* emotion,
                                                   double weight) {
  return Invoke([&] {
    Unwrap(input).AddSegmentEmotion(SegmentArgument(segment),
                                    EndpointArgument(at),
                                    Utf8Argument(emotion, "emotion"), weight);
  });
}

thespeon_status thespeon_input_segment_set_speed(thespeon_input* input,
                                                 uint64_t segment,
                                                 thespeon_endpoint at,
                                                 double speed) {
  return Invoke([&] {
    Unwrap(input).SetSegmentSpeed(SegmentArgument(segment),
                                  EndpointArgument(at), speed);
  });
}

thespeon_status thespeon_input_segment_set_loudness(thespeon_input* input,
                                                    uint64_t segment,
                                                    thespeon_endpoint at,
                                                    double loudness) {
  return Invoke([&] {
    Unwrap(input).SetSegmentLoudness(SegmentArgument(segment),
                                     EndpointArgument(at), loudness);
  });
}

thespeon_status thespeon_synthesize_wav(const char* data_directory,
                                        const thespeon_input* input,
                                        const char* output_path,
                                        thespeon_cancel_token* cancel) {
  return Invoke([&] {
    thespeon::Engine(Utf8Path(data_directory, "data_directory"))
        .Synthesize(Unwrap(input).Build(),
                    Utf8Path(output_path, "output_path"), TokenOf(cancel));
  });
}

thespeon_status thespeon_synthesize_stream(
    const char* data_directory, const thespeon_input* input,
    thespeon_audio_callback callback, void* user_data,
    thespeon_cancel_token* cancel) {
  return Invoke([&] {
    thespeon::Engine engine(Utf8Path(data_directory, "data_directory"));
    StreamThrough(engine, Unwrap(input).Build(), callback, nullptr, user_data,
                  TokenOf(cancel));
  });
}

thespeon_status thespeon_synthesize_stream_ex(
    const char* data_directory, const thespeon_input* input,
    thespeon_audio_callback on_audio, thespeon_sample_callback on_samples,
    void* user_data, thespeon_cancel_token* cancel) {
  return Invoke([&] {
    thespeon::Engine engine(Utf8Path(data_directory, "data_directory"));
    StreamThrough(engine, Unwrap(input).Build(), on_audio, on_samples,
                  user_data, TokenOf(cancel));
  });
}

thespeon_status thespeon_set_inference_options(int32_t thread_count,
                                               uint32_t flags) {
  return Invoke([&] {
    if (thread_count < 0) throw InvalidArgument("thread_count is negative");
    if ((flags & ~(THESPEON_INFERENCE_SPINNING |
                   THESPEON_INFERENCE_LOW_MEMORY)) != 0)
      throw InvalidArgument("flags has unknown bits");
    thespeon::EngineOptions config;
    config.intra_op_threads = thread_count;
    config.allow_spinning = (flags & THESPEON_INFERENCE_SPINNING) != 0;
    config.low_memory = (flags & THESPEON_INFERENCE_LOW_MEMORY) != 0;
    thespeon::SetEngineOptions(config);
  });
}

thespeon_status thespeon_engine_create(const char* data_directory,
                                       thespeon_engine** out_engine) {
  return Invoke([&] {
    if (out_engine == nullptr) throw InvalidArgument("out_engine is null");
    *out_engine = nullptr;
    auto engine = std::make_unique<thespeon::Engine>(
        Utf8Path(data_directory, "data_directory"));
    *out_engine = reinterpret_cast<thespeon_engine*>(engine.release());
  });
}

void thespeon_engine_destroy(thespeon_engine* engine) {
  delete reinterpret_cast<thespeon::Engine*>(engine);
}

thespeon_status thespeon_engine_preload(
    thespeon_engine* engine, const char* module_identifier, uint32_t flags,
    thespeon_progress_callback progress, void* user_data,
    thespeon_cancel_token* cancel) {
  return PreloadWith(progress, user_data, [&](const auto& forward) {
    Unwrap(engine).Preload(Utf8Argument(module_identifier, "module_identifier"),
                           (flags & THESPEON_PRELOAD_WARMUP) != 0, forward,
                           TokenOf(cancel));
  });
}

thespeon_status thespeon_engine_preload_input(
    thespeon_engine* engine, const thespeon_input* input, uint32_t flags,
    thespeon_progress_callback progress, void* user_data,
    thespeon_cancel_token* cancel) {
  return PreloadWith(progress, user_data, [&](const auto& forward) {
    Unwrap(engine).Preload(Unwrap(input).Build(),
                           (flags & THESPEON_PRELOAD_WARMUP) != 0, forward,
                           TokenOf(cancel));
  });
}

thespeon_status thespeon_engine_unload(thespeon_engine* engine,
                                       const char* module_identifier) {
  return Invoke([&] {
    Unwrap(engine).Unload(Utf8Argument(module_identifier, "module_identifier"));
  });
}

thespeon_status thespeon_engine_unload_input(thespeon_engine* engine,
                                             const thespeon_input* input) {
  return Invoke([&] { Unwrap(engine).Unload(Unwrap(input).Build()); });
}

thespeon_status thespeon_engine_is_loaded(thespeon_engine* engine,
                                          const char* module_identifier,
                                          uint8_t* out_loaded) {
  return Invoke([&] {
    if (out_loaded == nullptr) throw InvalidArgument("out_loaded is null");
    *out_loaded = 0;
    *out_loaded = Unwrap(engine).IsLoaded(
                      Utf8Argument(module_identifier, "module_identifier"))
                      ? 1
                      : 0;
  });
}

thespeon_status thespeon_engine_is_input_loaded(thespeon_engine* engine,
                                                const thespeon_input* input,
                                                uint8_t* out_loaded) {
  return Invoke([&] {
    if (out_loaded == nullptr) throw InvalidArgument("out_loaded is null");
    *out_loaded = 0;
    *out_loaded = Unwrap(engine).IsLoaded(Unwrap(input).Build()) ? 1 : 0;
  });
}

thespeon_status thespeon_engine_list_loaded(thespeon_engine* engine,
                                            thespeon_module_list** out_modules) {
  return Invoke([&] {
    if (out_modules == nullptr) throw InvalidArgument("out_modules is null");
    *out_modules = nullptr;
    *out_modules = new ModuleListStorage(Unwrap(engine).GetAllLoaded());
  });
}

thespeon_status thespeon_engine_refresh(thespeon_engine* engine) {
  return Invoke([&] { Unwrap(engine).Refresh(); });
}

thespeon_status thespeon_engine_resident_bytes(thespeon_engine* engine,
                                               uint64_t* out_bytes) {
  return Invoke([&] {
    if (out_bytes == nullptr) throw InvalidArgument("out_bytes is null");
    *out_bytes = static_cast<uint64_t>(Unwrap(engine).ResidentBytes());
  });
}

thespeon_status thespeon_engine_list_models(thespeon_engine* engine,
                                            thespeon_catalog** out_catalog) {
  return Invoke([&] {
    if (out_catalog == nullptr) throw InvalidArgument("out_catalog is null");
    *out_catalog = nullptr;
    *out_catalog = NewCatalog(Unwrap(engine).catalog());
  });
}

thespeon_status thespeon_engine_validate_input(thespeon_engine* engine,
                                               const thespeon_input* input) {
  return Invoke([&] {
    thespeon::ValidateInput(Unwrap(engine).catalog(), Unwrap(input).Build());
  });
}

thespeon_status thespeon_engine_delete_model(thespeon_engine* engine,
                                             const char* model,
                                             const char* module_type,
                                             const char* module_version,
                                             uint8_t force,
                                             thespeon_delete_result* result) {
  return Invoke([&] {
    ClearDeleteResult(result);
    StoreDeleteResult(
        Unwrap(engine).Remove(Utf8Argument(model, "model"),
                              OptionalArgument(module_type, "module_type"),
                              Narrowing(module_type, module_version),
                              force != 0, false),
        result);
  });
}

thespeon_status thespeon_engine_delete_all_models(
    thespeon_engine* engine, thespeon_delete_result* result) {
  return Invoke([&] {
    ClearDeleteResult(result);
    StoreDeleteResult(Unwrap(engine).RemoveAll(false), result);
  });
}

thespeon_status thespeon_engine_synthesize_wav(thespeon_engine* engine,
                                               const thespeon_input* input,
                                               const char* output_path,
                                               thespeon_cancel_token* cancel) {
  return Invoke([&] {
    Unwrap(engine).Synthesize(Unwrap(input).Build(),
                              Utf8Path(output_path, "output_path"),
                              TokenOf(cancel));
  });
}

thespeon_status thespeon_engine_synthesize_stream(
    thespeon_engine* engine, const thespeon_input* input,
    thespeon_audio_callback callback, void* user_data,
    thespeon_cancel_token* cancel) {
  return Invoke([&] {
    StreamThrough(Unwrap(engine), Unwrap(input).Build(), callback, nullptr,
                  user_data, TokenOf(cancel));
  });
}

thespeon_status thespeon_engine_synthesize_stream_ex(
    thespeon_engine* engine, const thespeon_input* input,
    thespeon_audio_callback on_audio, thespeon_sample_callback on_samples,
    void* user_data, thespeon_cancel_token* cancel) {
  return Invoke([&] {
    StreamThrough(Unwrap(engine), Unwrap(input).Build(), on_audio, on_samples,
                  user_data, TokenOf(cancel));
  });
}

const char* thespeon_get_last_error(void) { return last_error.c_str(); }

void thespeon_string_free(char* value) { delete[] value; }

void thespeon_catalog_free(thespeon_catalog* catalog) {
  delete static_cast<CatalogStorage*>(catalog);
}

void thespeon_module_list_free(thespeon_module_list* modules) {
  delete static_cast<ModuleListStorage*>(modules);
}

}  // extern "C"

// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#ifndef THESPEON_H
#define THESPEON_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(THESPEON_C_ABI_BUILD)
#    define THESPEON_API __declspec(dllexport)
#  else
#    define THESPEON_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define THESPEON_API __attribute__((visibility("default")))
#else
#  define THESPEON_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define THESPEON_ABI_VERSION UINT32_C(1)

typedef int32_t thespeon_status;

#define THESPEON_STATUS_OK ((thespeon_status)0)
#define THESPEON_STATUS_INVALID_ARGUMENT ((thespeon_status)1)
#define THESPEON_STATUS_GRAPH_ERROR ((thespeon_status)2)
#define THESPEON_STATUS_RUNTIME_ERROR ((thespeon_status)3)
#define THESPEON_STATUS_CANCELLED ((thespeon_status)4)

#define THESPEON_PAUSE "\xE2\x8F\xB8"                 /* U+23F8 */
#define THESPEON_AUDIO_SAMPLE_REQUEST "\xE2\x97\x8E"  /* U+25CE */

typedef struct thespeon_import_result {
  uint64_t written;
  uint64_t unchanged;
} thespeon_import_result;

/* Filled by every delete call and by thespeon_delete_preview, which reports
   what a delete would do without doing it. */
typedef struct thespeon_delete_result {
  uint64_t binaries_removed;
  uint64_t bytes_freed;
  uint64_t binaries_kept;       /* still needed by a module that stays */
  uint64_t stranded_characters; /* left with no language module */
  uint64_t problems;            /* configs that could not be read */
  uint8_t  swept;               /* 0 when problems blocked the sweep, leaving
                                   binaries_removed and bytes_freed at 0 */
} thespeon_delete_result;

/* Catalog strings are never null; a field the module leaves unset is "". An
   array whose count is 0 may be null. */
typedef struct thespeon_module_language {
  const char* iso639_2;
  const char* iso639_3;
  const char* glottocode;
  const char* iso3166_1;
  const char* iso3166_2;
  const char* custom_dialect;
  const char* name_in_english;
  const char* autonym;
} thespeon_module_language;

typedef struct thespeon_version {
  int32_t major;
  int32_t minor;
  int32_t patch;
} thespeon_version;

/* An emotion the character accepts. Pass name to the input's emotion setters;
   guide describes the emotion for the person choosing it. */
typedef struct thespeon_emotion_description {
  const char* name;
  const char* guide;
} thespeon_emotion_description;

typedef struct thespeon_character_module {
  const char* identifier; /* "" in packs built before the field existed */
  const char* name;
  const char* module_type; /* "XS" through "XL" */
  thespeon_version version;
  const thespeon_module_language* languages;
  uint64_t language_count;
  const thespeon_emotion_description* emotions;
  uint64_t emotion_count;
} thespeon_character_module;

typedef struct thespeon_language_module {
  const char* identifier;
  const char* name;
  thespeon_version version;
  const thespeon_module_language* languages;
  uint64_t language_count;
} thespeon_language_module;

/* The installed modules, as one allocation. Release it with
   thespeon_catalog_free(). */
typedef struct thespeon_catalog {
  const thespeon_character_module* character_modules;
  uint64_t character_module_count;
  const thespeon_language_module* language_modules;
  uint64_t language_module_count;
  const char* const* problems; /* configs that could not be read */
  uint64_t problem_count;
} thespeon_catalog;

/* One allocation; release it with thespeon_module_list_free(). */
typedef struct thespeon_module_list {
  const thespeon_character_module* character_modules;
  uint64_t character_module_count;
  const thespeon_language_module* language_modules;
  uint64_t language_module_count;
} thespeon_module_list;

/* Samples are mono 44.1 kHz float data borrowed for the duration of the call.*/
typedef void (*thespeon_audio_callback)(const float* samples,
                                        uint64_t sample_count,
                                        uint8_t is_final,
                                        void* user_data);

/* Indices into the stream the audio callback delivers, one per
   THESPEON_AUDIO_SAMPLE_REQUEST marker, in left-to-right text order. The array
   is borrowed for the duration of the call. */
typedef void (*thespeon_sample_callback)(const int64_t* sample_indices,
                                         uint64_t count,
                                         void* user_data);

/* Returns the runtime ABI version for comparison with THESPEON_ABI_VERSION. */
THESPEON_API uint32_t thespeon_get_abi_version(void);

/* Imports one .lingotion pack into an explicit model data directory. */
THESPEON_API thespeon_status thespeon_import_pack(
    const char* data_directory, const char* pack_path,
    thespeon_import_result* result);

THESPEON_API thespeon_status thespeon_list_models(
    const char* data_directory, thespeon_catalog** out_catalog);

/* Reports what deleting a module would do, changing nothing. model names a
   character or a language module, by module identifier or by display name;
   pass null for every installed module. module_type applies only to characters,
   and it and module_version may be null. */
THESPEON_API thespeon_status thespeon_delete_preview(
    const char* data_directory, const char* model, const char* module_type,
    const char* module_version, thespeon_delete_result* result);

/* Deletes one module and every binary no remaining module needs. Deleting a
   language module that a remaining character depends on fails unless force is
   non-zero; thespeon_get_last_error() then names those characters. */
THESPEON_API thespeon_status thespeon_delete_model(
    const char* data_directory, const char* model, const char* module_type,
    const char* module_version, uint8_t force, thespeon_delete_result* result);

/* Deletes every installed module. */
THESPEON_API thespeon_status thespeon_delete_all_models(
    const char* data_directory, thespeon_delete_result* result);

/* Stops a blocking call running on another thread, which then returns
   THESPEON_STATUS_CANCELLED; the engine stays usable and loaded. Null means
   uncancellable. Cancelling is one-way, so use a fresh token per call. Destroy
   is safe while a cancelled call unwinds, but like thespeon_engine_destroy
   must not race another call on that token. */
typedef struct thespeon_cancel_token thespeon_cancel_token;

THESPEON_API thespeon_status thespeon_cancel_token_create(
    thespeon_cancel_token** out_token);

/* Callable from any thread, at any time, including before the call starts. */
THESPEON_API void thespeon_cancel_token_cancel(thespeon_cancel_token* token);

THESPEON_API uint8_t thespeon_cancel_token_is_cancelled(
    const thespeon_cancel_token* token);

/* Passing null is allowed. */
THESPEON_API void thespeon_cancel_token_destroy(thespeon_cancel_token* token);

/* One synthesis input, which names its module by identifier or by character
   name and carries the text as segments. A handle must not be used from two
   threads at once, but may be reused across calls. */
typedef struct thespeon_input thespeon_input;

/* iso639_2 is required; the rest may be null. */
typedef struct thespeon_language {
  const char* iso639_2;
  const char* iso639_3;
  const char* glottocode;
  const char* iso3166_1;
  const char* iso3166_2;
  const char* custom_dialect;
} thespeon_language;

typedef int32_t thespeon_endpoint;

#define THESPEON_ENDPOINT_START ((thespeon_endpoint)0)
#define THESPEON_ENDPOINT_END ((thespeon_endpoint)1)

/* Starts with version 1.0.0, English as the default language, no segments. */
THESPEON_API thespeon_status thespeon_input_create(thespeon_input** out_input);

/* Validates input_json, which the builder's setters may then amend. */
THESPEON_API thespeon_status thespeon_input_from_json(
    const char* input_json, thespeon_input** out_input);

/* Passing null is allowed. */
THESPEON_API void thespeon_input_destroy(thespeon_input* input);

/* Validates the document as the synthesis calls would. Release it with
   thespeon_string_free(). */
THESPEON_API thespeon_status thespeon_input_to_json(
    const thespeon_input* input, char** result_json);

/* The character-module and character setters replace each other. */
THESPEON_API thespeon_status thespeon_input_set_character_module(
    thespeon_input* input, const char* module_identifier);

/* module_type and module_version may be null. */
THESPEON_API thespeon_status thespeon_input_set_character(
    thespeon_input* input, const char* character_name, const char* module_type,
    const char* module_version);

/* Null unsets. */
THESPEON_API thespeon_status thespeon_input_set_default_emotion(
    thespeon_input* input, const char* emotion);

THESPEON_API thespeon_status thespeon_input_set_default_language(
    thespeon_input* input, const thespeon_language* language);

/* out_index may be null. */
THESPEON_API thespeon_status thespeon_input_add_segment(
    thespeon_input* input, const char* text, uint64_t* out_index);

/* The segment's text is then IPA. */
THESPEON_API thespeon_status thespeon_input_segment_set_custom_pronounced(
    thespeon_input* input, uint64_t segment, uint8_t enabled);

/* Null unsets. */
THESPEON_API thespeon_status thespeon_input_segment_set_language(
    thespeon_input* input, uint64_t segment, const thespeon_language* language);

/* Single-name shorthand for any endpoint without a blend. Null unsets. */
THESPEON_API thespeon_status thespeon_input_segment_set_emotion(
    thespeon_input* input, uint64_t segment, const char* emotion);

/* Adds one emotion to an endpoint's blend; the same name overwrites. */
THESPEON_API thespeon_status thespeon_input_segment_add_emotion(
    thespeon_input* input, uint64_t segment, thespeon_endpoint at,
    const char* emotion, double weight);

THESPEON_API thespeon_status thespeon_input_segment_set_speed(
    thespeon_input* input, uint64_t segment, thespeon_endpoint at,
    double speed);

THESPEON_API thespeon_status thespeon_input_segment_set_loudness(
    thespeon_input* input, uint64_t segment, thespeon_endpoint at,
    double loudness);

/* THESPEON_STATUS_OK when synthesis would accept the input, which includes
   every emotion it names being one the character lists. Runs no model, so a
   word the language module cannot pronounce is not caught. */
THESPEON_API thespeon_status thespeon_validate_input(
    const char* data_directory, const thespeon_input* input);

/* Synchronously synthesizes a mono 44.1 kHz PCM16 WAV file. A cancelled call
   leaves output_path untouched. */
THESPEON_API thespeon_status thespeon_synthesize_wav(
    const char* data_directory, const thespeon_input* input,
    const char* output_path, thespeon_cancel_token* cancel);

/* Synchronously synthesizes audio and invokes callback on the calling thread. */
THESPEON_API thespeon_status thespeon_synthesize_stream(
    const char* data_directory, const thespeon_input* input,
    thespeon_audio_callback callback, void* user_data,
    thespeon_cancel_token* cancel);

/* As thespeon_synthesize_stream, and additionally reports the audio sample
   requests the text asked for. on_samples may be null. */
THESPEON_API thespeon_status thespeon_synthesize_stream_ex(
    const char* data_directory, const thespeon_input* input,
    thespeon_audio_callback on_audio, thespeon_sample_callback on_samples,
    void* user_data, thespeon_cancel_token* cancel);

/* An engine keeps loaded models alive between calls. It creates no threads:
   every call blocks on the calling thread, and the caller owns its threading.
   One engine may be used from several threads at once, but thespeon_engine_destroy
   must not run concurrently with any other call on that engine. */
typedef struct thespeon_engine thespeon_engine;

#define THESPEON_INFERENCE_SPINNING UINT32_C(1)
#define THESPEON_INFERENCE_LOW_MEMORY UINT32_C(2)

/* Applies to every engine created afterwards and fails while one exists,
   including the one a standalone synthesize call creates. thread_count 0
   selects the runtime default. */
THESPEON_API thespeon_status thespeon_set_inference_options(
    int32_t thread_count, uint32_t flags);

/* Loads the ONNX sessions only. Cheap, but leaves the first synthesis paying
   the cost of each model's first run. */
#define THESPEON_PRELOAD_SESSIONS UINT32_C(0)
/* Also runs a throwaway synthesis so the first real one reaches steady-state
   latency. Costs about one utterance, and is what actually cuts the wait. */
#define THESPEON_PRELOAD_WARMUP UINT32_C(1)

/* Return 0 to cancel the preload. */
typedef uint8_t (*thespeon_progress_callback)(uint64_t done, uint64_t total,
                                              void* user_data);

THESPEON_API thespeon_status thespeon_engine_create(
    const char* data_directory, thespeon_engine** out_engine);

THESPEON_API void thespeon_engine_destroy(thespeon_engine* engine);

/* Blocks until the character module is loaded. Synthesis works without this,
   only slower. progress may be null. */
THESPEON_API thespeon_status thespeon_engine_preload(
    thespeon_engine* engine, const char* module_identifier, uint32_t flags,
    thespeon_progress_callback progress, void* user_data,
    thespeon_cancel_token* cancel);

/* As thespeon_engine_preload, taking the character and languages from an
   input, so any input for that character will do. */
THESPEON_API thespeon_status thespeon_engine_preload_input(
    thespeon_engine* engine, const thespeon_input* input, uint32_t flags,
    thespeon_progress_callback progress, void* user_data,
    thespeon_cancel_token* cancel);

/* Releases one model's sessions. Safe during synthesis of that model: the
   memory is freed once the in-flight run finishes. */
THESPEON_API thespeon_status thespeon_engine_unload(
    thespeon_engine* engine, const char* module_identifier);

THESPEON_API thespeon_status thespeon_engine_unload_input(
    thespeon_engine* engine, const thespeon_input* input);

/* Whether the module's sessions are resident. module_identifier names a
   character or a language module, and a character counts as loaded whatever
   languages it was preloaded with. Loads no sessions, but the first check of a
   module after the catalog is read parses its config and graph. */
THESPEON_API thespeon_status thespeon_engine_is_loaded(
    thespeon_engine* engine, const char* module_identifier,
    uint8_t* out_loaded);

/* Whether thespeon_engine_preload_input of this input would load nothing. */
THESPEON_API thespeon_status thespeon_engine_is_input_loaded(
    thespeon_engine* engine, const thespeon_input* input, uint8_t* out_loaded);

/* Every module thespeon_engine_is_loaded reports as loaded. A module whose
   files cannot be read is left out rather than failing the call. */
THESPEON_API thespeon_status thespeon_engine_list_loaded(
    thespeon_engine* engine, thespeon_module_list** out_modules);

/* Re-reads the catalog after thespeon_import_pack has written to the directory. */
THESPEON_API thespeon_status thespeon_engine_refresh(thespeon_engine* engine);

/* Total size of the models currently held in memory. */
THESPEON_API thespeon_status thespeon_engine_resident_bytes(
    thespeon_engine* engine, uint64_t* out_bytes);

THESPEON_API thespeon_status thespeon_engine_list_models(
    thespeon_engine* engine, thespeon_catalog** out_catalog);

/* As thespeon_validate_input, against the engine's catalog. */
THESPEON_API thespeon_status thespeon_engine_validate_input(
    thespeon_engine* engine, const thespeon_input* input);

/* As the calls of the same name above, on the engine's own data directory.
   Both drop every session the engine holds, since a deleted file must not stay
   open, and re-read the catalog. */
THESPEON_API thespeon_status thespeon_engine_delete_model(
    thespeon_engine* engine, const char* model, const char* module_type,
    const char* module_version, uint8_t force, thespeon_delete_result* result);

THESPEON_API thespeon_status thespeon_engine_delete_all_models(
    thespeon_engine* engine, thespeon_delete_result* result);

THESPEON_API thespeon_status thespeon_engine_synthesize_wav(
    thespeon_engine* engine, const thespeon_input* input,
    const char* output_path,
    thespeon_cancel_token* cancel);

THESPEON_API thespeon_status thespeon_engine_synthesize_stream(
    thespeon_engine* engine, const thespeon_input* input,
    thespeon_audio_callback callback, void* user_data,
    thespeon_cancel_token* cancel);

/* on_samples may be null. */
THESPEON_API thespeon_status thespeon_engine_synthesize_stream_ex(
    thespeon_engine* engine, const thespeon_input* input,
    thespeon_audio_callback on_audio, thespeon_sample_callback on_samples,
    void* user_data, thespeon_cancel_token* cancel);

/* Returns thread-local text valid until the next operation on this thread. */
THESPEON_API const char* thespeon_get_last_error(void);

/* Releases strings allocated by this ABI. Passing null is allowed. */
THESPEON_API void thespeon_string_free(char* value);
THESPEON_API void thespeon_catalog_free(thespeon_catalog* catalog);
THESPEON_API void thespeon_module_list_free(thespeon_module_list* modules);

#ifdef __cplusplus
}
#endif

#endif

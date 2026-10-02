# libthespeon

libthespeon runs Lingotion Thespeon, a neural text-to-speech engine, locally on-device using the ONNX Runtime. It is a C shared library, plus a CLI for trying out voices and benchmarking.

## Concepts

- **Character module**: a voice. It comes in size tiers (`XS`, `S`, `M`, `L`, `XL`), where larger means higher acting fidelity but with higher performance requirements, and is identified either by its module identifier or by character name, module type and version.
- **Language module**: converts text in one language to phonemes. A character needs a language module for each language it speaks.
- **Lingotion file** (`.lingotion`): a collection of module binaries. Imported files are unpacked into a data directory, which every other call reads from.

This is a beta version, so please be aware that nothing in this repo can as of yet be considered stable and may change between releases.

Supported targets: Linux x86-64, macOS arm64, and Windows x64. Output is mono 44.1 kHz audio.

See [known-issues.md](./known-issues.md) for unimplemented features and known bugs, and [CONTRIBUTING.md](./CONTRIBUTING.md) for tests and profiling.

## Build

```bash
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On Windows, use `cmake -S . -B build -A x64` and `cmake --build build --config Release --parallel` from a Visual Studio Developer PowerShell. Build options:

- `THESPEON_BUILD_CLI=OFF`: omit the CLI and miniaudio.
- `THESPEON_BUILD_C_ABI=OFF`: omit the C ABI.
- `THESPEON_BUILD_TESTS=OFF`: omit the unit tests and GoogleTest. On by default when libthespeon is the top-level project.
- `THESPEON_PROFILER=tracy`: instrument with the [Tracy](https://github.com/wolfpld/tracy) profiler. Defaults to `none`, which compiles the profiling macros away.

Requires CMake 3.24 or later, a C++20 compiler, and network access on first configure: dependencies, including ONNX Runtime 1.24.1, are pinned and fetched by CMake.

### Linking

The whole ABI is the single header [`src/abi/thespeon.h`](src/abi/thespeon.h), which depends only on `<stdint.h>`. Copy it into your project, `#include "thespeon.h"`, and link against the library. The build writes the library to `build/` (`build/Release/` with Visual Studio), next to the ONNX Runtime library it depends on:

| Platform | Library | ONNX Runtime |
| --- | --- | --- |
| Linux | `libthespeon.so.1` | `libonnxruntime.so.1` |
| macOS | `libthespeon.1.dylib` | `libonnxruntime.1.24.1.dylib` |
| Windows | `thespeon.dll`, import library `thespeon.lib` | `onnxruntime.dll` |

Ship both files in the same directory; libthespeon finds ONNX Runtime beside itself.

From CMake, you can instead add this repository, with its submodules, as a subdirectory and link the `thespeon_c` target, which carries the include path:

```cmake
set(THESPEON_BUILD_CLI OFF)
add_subdirectory(libthespeon)
target_link_libraries(my_app PRIVATE thespeon_c)
```

## CLI

The CLI can either synthesize to a .wav file, or stream audio directly to the standard audio output device.

```bash
thespeon [--data-dir DIR] import path/to/file.lingotion
thespeon [--data-dir DIR] models [--json]
thespeon [--data-dir DIR] emotions --model NAME [--verbose | -v] [--json]
thespeon [--data-dir DIR] delete --model NAME [--force] [--dry-run]
thespeon [--data-dir DIR] delete --all [--force] [--dry-run]
thespeon [--data-dir DIR] synthesize <input> [--output FILE | --play] [inference options]
thespeon [--data-dir DIR] validate <input>
thespeon [--data-dir DIR] bench <input> [--repeats N] [inference options]
thespeon [--data-dir DIR] bench <input> --sweep [--repeats N]
```

`<input>` is one of:

- `--model NAME --text TEXT [--emotion EMOTION]`, where `--emotion` sets one emotion for the whole text
- `--input JSON`, a [synthesis input](#synthesis-input) document
- `--input-file FILE`, the same document read from a file, or `-` for stdin

`emotions` lists the emotion names a character accepts; `--verbose` adds a short guide to each. `validate` checks an input as synthesis would, without running a model, and exits 1 with the reason if it would be rejected.

`--model`, in `<input>` and in `emotions`, accepts a character name or a module identifier. Narrow a name with `--module-type` (`XS`, `S`, `M`, `L` or `XL`) and `--module-version`; otherwise the largest, newest matching character module is selected.

Unless `--data-dir` is given, the CLI uses a default data directory in the platform's user-data location:

- Windows: `%LOCALAPPDATA%\metagraph-cpp\models`, falling back to `%APPDATA%`
- Linux: `$XDG_DATA_HOME/metagraph-cpp/models`, falling back to `~/.local/share/metagraph-cpp/models`
- macOS: `~/Library/Application Support/metagraph-cpp/models`

### Inference options

`synthesize` (including `--play`) and `bench` accept:

| Option | Default | Effect |
| --- | --- | --- |
| `--threads N` | `0` | Threads inference runs on. `0` lets ONNX Runtime pick, usually one per physical core. |
| `--spinning` | off | Idle threads spin before sleeping, which can lower latency but uses far more CPU. |
| `--low-memory` | off | Turns off ONNX Runtime's memory arena and memory planning, significantly reducing peak memory, at a possible cost in speed. |

All models share one process-wide thread pool, so `--threads` caps the total number of inference threads. Use `bench` to compare settings on your hardware:

```bash
thespeon bench --model NAME --text TEXT --threads 4
thespeon bench --model NAME --text TEXT --threads 4 --spinning
thespeon synthesize --model NAME --text TEXT --low-memory --output out.wav
```

`bench --sweep` does this for you: it times every thread count from 1 up to the core count, plus the default, with and without spinning, then tries low memory on the best of them. It prints the fastest set and a recommended one, which is the least CPU within 10% of the fastest, as CLI flags and as the matching `thespeon_set_inference_options` call. Each set gets `--repeats` warm runs (default 3), so a sweep takes a few minutes.

## C ABI

For integrations, we provide a C ABI that is used to interface with the compiled library. Synthesis can run as a one-shot (`thespeon_synthesize_wav`, `thespeon_synthesize_stream`), which loads the models on every call. We recommend creating a `thespeon_engine` instead, a handle that keeps models loaded between calls.

The C ABI has no default data directory: every call takes one explicitly, so your application decides where its models live.

- use `thespeon_engine_preload(..., THESPEON_PRELOAD_WARMUP, ...)` to move load and first-run costs off the first synthesis;
- use `thespeon_engine_unload` when a character is no longer needed;
- `thespeon_engine_preload_input` and `thespeon_engine_unload_input` do the same for the character and language a synthesis input selects;
- call `thespeon_engine_refresh` after importing into an engine's data directory;
- query retained model memory with `thespeon_engine_resident_bytes`;
- check an input without synthesizing with `thespeon_engine_validate_input` (or `thespeon_validate_input`); it runs no model, so words the language module cannot pronounce are not caught;
- call `thespeon_set_inference_options` before creating engines to set the thread count, spinning and low-memory mode for the whole process.

### Threading

Every call blocks on the calling thread, and callbacks run synchronously on that thread before the call returns. Audio buffers and sample-index arrays are borrowed for the callback duration. The only other threads are ONNX Runtime's inference thread pool. There is one per process, shared by every engine and model, so concurrent syntheses compete for the same threads. It is created with the first engine, using the options from `thespeon_set_inference_options`, and released with the last one; that is why the options cannot change while an engine exists.

- An engine may be used from several threads at once, including concurrent syntheses; `thespeon_engine_destroy` must not run alongside any other call on it.
- A `thespeon_input` must not be used from two threads at once, but may be reused across calls.
- `thespeon_cancel_token_cancel` may be called from any thread.

### Cancellation

Synthesis and preload calls take an optional `thespeon_cancel_token*` - pass null for an uncancellable call. Calling `thespeon_cancel_token_cancel` from another thread makes the call return `THESPEON_STATUS_CANCELLED`, and the engine stays usable with its models loaded. A cancelled `synthesize_wav` leaves the output file untouched. A preload can also be cancelled by returning 0 from its progress callback.

Cancelling is one-way, so create a fresh token per call.

### Errors

Every call that can fail returns a `thespeon_status`:

| Status | Meaning |
| --- | --- |
| `THESPEON_STATUS_OK` | Success. |
| `THESPEON_STATUS_INVALID_ARGUMENT` | A bad argument or input document. |
| `THESPEON_STATUS_GRAPH_ERROR` | The model rejected the input during synthesis. |
| `THESPEON_STATUS_RUNTIME_ERROR` | Anything else, such as a module that is not installed, or an I/O or ONNX Runtime failure. |
| `THESPEON_STATUS_CANCELLED` | A cancel token stopped the call. |

`thespeon_get_last_error()` returns the message for the most recent call on the current thread, or `""` after a success. It is never null, and the pointer is valid until the next call on that thread, so copy it if you need to keep it.

### Ownership

The library copies every string you pass in and keeps no pointers into your memory after a call returns. All strings, including paths, are UTF-8.

Anything it returns is yours to release: a `thespeon_X` handle with `thespeon_X_destroy`, and strings, catalogs and module lists with the `_free` function the header names beside them. Release functions accept null, and output parameters are null after a failure.

### Streaming

`thespeon_synthesize_stream` and `thespeon_engine_synthesize_stream` deliver audio in chunks, as mono 44.1 kHz float samples, to an audio callback. The chunk with `is_final` set is the last one.

The `_ex` synthesis variants take a second callback, which is called before any audio is returned. The variant lets the input text at any point contain an Audio Sample Request (ASR) character. The callback then recieves an array of audio sample indices: element `i` is the audio sample index corresponding to when the `i`-th ASR character is played, counted from the first sample of the whole stream, across segments.

## Synthesis input

CLI `--input`/`--input-file` and `thespeon_input_from_json` accept the same JSON document:

```json
{
  "version": "1.0.0",
  "moduleIdentifier": "61469f1e34796be9a253e59f802bf498",
  "defaultLanguage": {"iso639_2": "eng", "iso3166_1": "US"},
  "defaultEmotion": "Interest",
  "segments": [
    {"text": "The first segment."},
    {"text": "And the second.", "emotion": "Joy"},
    {"text": "The third.", "endEmotion": {"Joy": 0.5, "Surprise": 0.5}}
  ]
}
```

Required fields are `version` (currently `1.0.0`), `defaultLanguage`, a non-empty `segments` array, and either `moduleIdentifier` or `characterName`, `moduleType` and `moduleVersion`.

Segment fields:

- `text`, the text to be synthesized
- `language`, a language object that this specific text should be spoken in. If omitted, `defaultLanguage` is used.
- `isCustomPronounced`, when true, the text is [IPA](https://en.wikipedia.org/wiki/International_Phonetic_Alphabet) and is pronounced as written.
- `emotion`, one emotion name for the whole segment.
- `startEmotion`, `endEmotion`, emotion blends (`{"Name": weight}`) interpolated linearly over the segment. Weights are normalized to sum to 1. Either one overrides `emotion` at its end of the segment.
- `startSpeed`, `endSpeed`, the speech rate at the segment's start and end, interpolated linearly between them. `1.0` is normal.
- `startLoudness`, `endLoudness`, the same for loudness.

Emotion names are matched case-insensitively and must be `none` or one the character lists: see `thespeon emotions`, or the `emotions` array of each character module in the catalog. Segments without an emotion use `defaultEmotion` if it is set ([not yet fully, see known issues](./known-issues.md#defaultemotion-is-ignored-once-any-segment-sets-an-emotion)). Otherwise they take it from their neighbors: interpolated between the nearest segments that set one, and held constant before the first and after the last. A document that sets no emotion at all uses the character module's default (`Interest` in current modules). Speed and loudness default to `1.0` and must be finite and positive.

Invalid documents return `THESPEON_STATUS_INVALID_ARGUMENT` with a JSON-path error.

### Building input from C

The `thespeon_input` builder assembles the document without a JSON serializer. The synthesis, preload, and unload calls take the handle. `thespeon_input_from_json` turns an existing document into one, and `thespeon_input_to_json` validates the handle and returns its document:

```c
thespeon_engine* engine;
thespeon_input* input;
uint64_t segment;
thespeon_language us = {"eng", NULL, NULL, "US", NULL, NULL};

if (thespeon_engine_create(data_directory, &engine) != THESPEON_STATUS_OK) {
  fprintf(stderr, "%s\n", thespeon_get_last_error());
  return 1;
}
thespeon_input_create(&input);
thespeon_input_set_character_module(input, "61469f1e34796be9a253e59f802bf498");
thespeon_input_set_default_language(input, &us);
thespeon_input_add_segment(input, "The first segment.", &segment);
thespeon_input_segment_add_emotion(input, segment, THESPEON_ENDPOINT_START,
                                   "Interest", 0.7);
thespeon_input_segment_add_emotion(input, segment, THESPEON_ENDPOINT_START,
                                   "Joy", 0.3);
thespeon_input_segment_set_speed(input, segment, THESPEON_ENDPOINT_END, 1.2);
thespeon_input_add_segment(input, "And the second.", &segment);
thespeon_input_segment_set_emotion(input, segment, "Joy");

if (thespeon_engine_synthesize_wav(engine, input, "out.wav", NULL) !=
    THESPEON_STATUS_OK)
  fprintf(stderr, "%s\n", thespeon_get_last_error());
thespeon_input_destroy(input);
thespeon_engine_destroy(engine);
```

Setters reject only malformed arguments; schema errors surface from `thespeon_input_to_json` and the calls that take the handle.

### Control characters

Specific characters can be entered in a text to influence the synthesis.

| Character | C macro | Meaning |
| --- | --- | --- |
| `⏸` U+23F8 | `THESPEON_PAUSE` | Insert a short model-level pause. |
| `◎` U+25CE | `THESPEON_AUDIO_SAMPLE_REQUEST` | Request the output sample index at this position. |

The audio sample request character is particularly useful, as it can be used to synchronize audio output with events. See [Streaming](#streaming).

# metagraph-cpp

A C++20 runtime for executing Lingotion models.

The project provides two libraries:

- `metagraph::runner` loads a `.metagraph` file and executes it against a directory of `.onnx` models.
- `metagraph::textprep` runs a `.textprep` file, which describes text normalization, number expansion and word splitting for a language. It depends only on Protobuf.

Supported platforms are Linux x86_64, Apple Silicon macOS and Windows x64.

## Build

Requires CMake 3.24 or newer and a C++20 compiler. Protobuf and ONNX Runtime are downloaded and checksum-verified during configuration.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
```

On Windows, build from a Visual Studio Developer PowerShell with `-A x64`. On macOS, use a native arm64 CMake; Intel Macs are not supported.

## Use as a dependency

```cmake
add_subdirectory(metagraph-cpp)
target_link_libraries(my_executable PRIVATE metagraph::runner)
metagraph_stage_runtime(my_executable)
```

`metagraph_stage_runtime` copies the ONNX Runtime shared library next to the executable. Tools and tests are not built when the project is embedded; enable them with `-DMETAGRAPH_BUILD_TOOLS=ON` and `-DMETAGRAPH_BUILD_TESTS=ON`.

## Tools

The `tools` directory contains two example executables that run a graph on fixed inputs:

```bash
./build/run_lara -g <graph.metagraph> -m <model-directory> -o out.wav
./build/run_phonemizer -g <graph.metagraph> -m <model-directory>
```

`run_lara` runs a speech synthesis graph and writes a mono 44.1 kHz PCM16 WAV. `run_phonemizer` runs a phonemizer graph and prints the output tensor. Set `METAGRAPH_DEBUG=1` to print ONNX output shapes.

## Profiling

Configure with `-DMETAGRAPH_PROFILER=tracy` to instrument the runner with [Tracy](https://github.com/wolfpld/tracy) 0.14.1. Zones cover each graph node, each ONNX Runtime call and session creation. Use `tracy-capture` and `tracy-profiler` from the same Tracy release, as mismatched versions refuse to connect.

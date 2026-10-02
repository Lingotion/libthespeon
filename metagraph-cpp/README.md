# metagraph-cpp

A compact C++20 port of `model-meta-graph/src/metagraph_runner.py`, with a
standalone Lara example using ONNX Runtime on Linux x86_64 and Apple Silicon
macOS, plus native Windows x64.

## Build standalone

Install CMake 3.24 or newer and a C++20 compiler. CMake downloads and
checksum-verifies Protobuf 3.21.12 and ONNX Runtime 1.24.1 automatically. The
repository contains only generated C++ protobuf bindings; the schema itself
remains owned by `model-meta-graph`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Standalone builds create both `run_lara` and `run_phonemizer`, and stage the
platform's ONNX Runtime shared library beside each executable. Supported
targets are Linux x86_64, Apple Silicon macOS, and Windows x64.

On macOS, run CMake natively on Apple Silicon. The official ONNX Runtime 1.24.1
release does not provide an Intel macOS archive.

On Windows, build from a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
.\build\Release\run_lara.exe -g <graph.metagraph> -m <model-directory>
```

This first Windows step uses the CPU execution provider. OpenVINO NPU support
will be added separately.

## Embed with CMake

When this project is added with `add_subdirectory`, the standalone executables
are disabled by default. Link consumers to `metagraph::runner` and stage ONNX
Runtime beside an executable with:

```cmake
add_subdirectory(metagraph-cpp)
target_link_libraries(my_executable PRIVATE metagraph::runner)
metagraph_stage_runtime(my_executable)
```

Pass `-DMETAGRAPH_BUILD_TOOLS=ON` to build the runner executables while the
project is embedded.

After changing the upstream protocol, refresh the generated snapshot with:

```bash
protoc -I ../model-meta-graph/src --cpp_out=src \
  ../model-meta-graph/src/meta_graph.proto
```

## Run Lara

Choose a `.metagraph` whose referenced `.onnx` files are in the model directory:

```bash
./build/run_lara \
  -g ../model-meta-graph/models/binaries/1865fe035fbfb16b8a250a9b7c758b9c.metagraph \
  -m ../model-meta-graph/models/binaries \
  -o test_lara_cpp.wav
```

The executable uses the same inputs as the sibling repository's `run_lara.py`
and writes a mono 44.1 kHz PCM16 WAV. Set `METAGRAPH_DEBUG=1` to print ONNX
output shapes.

## Profiling

Profiling is off by default. Enable [Tracy](https://github.com/wolfpld/tracy)
0.14.1 with `METAGRAPH_PROFILER=tracy`; CMake then fetches the Tracy client:

```bash
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMETAGRAPH_PROFILER=tracy
cmake --build build-profile --parallel
tracy-capture -o run.tracy &
./build-profile/run_lara -g <graph.metagraph> -m <model-directory>
tracy-profiler run.tracy
```

Build `tracy-capture` and `tracy-profiler` from the same Tracy release as the
client; mismatched versions refuse to connect. The Statistics window lists
per-zone call count and total, mean, and max time. Zones cover each graph node
(`ExecuteNode`, named by node id), each ONNX Runtime call (`OrtRun`), and
session creation (`CreateSession`).

When embedded, the parent sets `METAGRAPH_PROFILER`, and an existing
`Tracy::TracyClient` target is reused instead of fetching a second copy.

Code is instrumented only through the `METAGRAPH_PROFILE_*` macros in
[`src/metagraph_profile.h`](src/metagraph_profile.h). To change backend, add a
branch there and a `METAGRAPH_PROFILER` value in `CMakeLists.txt`.

## Run the phonemizer

The phonemizer executable uses the same two input sequences as
`model-meta-graph/src/run_phonemizer.py` and prints the resulting token tensor:

```bash
./build/run_phonemizer \
  -g ../model-meta-graph/phonemizer.metagraph \
  -m ../thespeon-generator-service/modelfiles/binaries
```
